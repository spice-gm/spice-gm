/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2017 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#ifndef STREAM_CHANNEL_H_
#define STREAM_CHANNEL_H_

#include <spice/stream-device.h>

#include "red-channel.h"

#include "push-visibility.h"

/**
 * This type it's a RedChannel class which implement display
 * channel with input only by stream.
 * A pointer to StreamChannel can be converted to a RedChannel.
 */
struct StreamChannel;

/**
 * Create StreamChannel.
 */
red::shared_ptr<StreamChannel> stream_channel_new(RedsState *server, uint32_t id);

struct StreamMsgStartStop;

typedef void (*stream_channel_start_proc)(void *opaque, struct StreamMsgStartStop *start,
                                          StreamChannel *channel);

struct StreamQueueStat {
    uint32_t num_items;
    uint32_t size;
};

typedef void (*stream_channel_queue_stat_proc)(void *opaque, const StreamQueueStat *stats,
                                               StreamChannel *channel);

struct StreamDataItem;
struct StreamChannelClient;
struct StreamChannel final: public RedChannel
{
    friend struct StreamChannelClient;
    friend struct StreamDataItem;
    StreamChannel(RedsState *reds, uint32_t id);

    /**
     * Reset channel at initial state
     */
    void reset();

    void change_format(const struct StreamMsgFormat *fmt);
    void send_data(const void *data, size_t size, uint32_t mm_time);

    void register_start_cb(stream_channel_start_proc cb, void *opaque);
    void register_queue_stat_cb(stream_channel_queue_stat_proc cb, void *opaque);

private:
    void on_connect(RedClient *red_client, RedStream *stream,
                    int migration, RedChannelCapabilities *caps) override;

    inline void update_queue_stat(int32_t num_diff, int32_t size_diff);
    void request_new_stream(StreamMsgStartStop *start);

    /* current video stream id, <0 if not initialized or
     * we are not sending a stream */
    int stream_id = -1;
    /* size of the current video stream */
    unsigned width = 0, height = 0;

    StreamQueueStat queue_stat;

    /* callback to notify when a stream should be started or stopped */
    stream_channel_start_proc start_cb;
    void *start_opaque;

    /* callback to notify when queue statistics changes */
    stream_channel_queue_stat_proc queue_cb;
    void *queue_opaque;
};

#include "pop-visibility.h"

#endif /* STREAM_CHANNEL_H_ */
