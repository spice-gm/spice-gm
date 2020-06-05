/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2018 Red Hat, Inc.

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

#ifndef STREAM_DEVICE_H
#define STREAM_DEVICE_H

#include <spice/stream-device.h>

#include "display-limits.h"
#include "char-device.h"

#include "push-visibility.h"

/**
 * StreamDevice inherits from RedCharDevice.
 */
struct StreamDevice;

// forward declarations
struct StreamChannel;
struct CursorChannel;
struct StreamQueueStat;

struct StreamDeviceDisplayInfo {
    uint32_t stream_id;
    char device_address[MAX_DEVICE_ADDRESS_LEN];
    uint32_t device_display_id;
};

red::shared_ptr<StreamDevice> stream_device_connect(RedsState *reds, SpiceCharDeviceInstance *sin);

#define MAX_GUEST_CAPABILITIES_BYTES ((STREAM_CAP_END+7)/8)

class StreamDevice final: public RedCharDevice
{
public:
    StreamDevice(RedsState *reds, SpiceCharDeviceInstance *sin);

    /* Create channel for the streaming device.
     * If the channel already exists the function does nothing.
     */
    void create_channel();

    /**
     * Returns -1 if the StreamDevice doesn't have a channel yet.
     */
    int32_t get_stream_channel_id();

    const StreamDeviceDisplayInfo *get_device_display_info();

protected:
    ~StreamDevice();

private:
    StreamDevHeader hdr;
    uint8_t hdr_pos;
    union AllMessages {
        StreamMsgFormat format;
        StreamMsgCapabilities capabilities;
        StreamMsgCursorSet cursor_set;
        StreamMsgCursorMove cursor_move;
        StreamMsgDeviceDisplayInfo device_display_info;
        uint8_t buf[STREAM_MSG_CAPABILITIES_MAX_BYTES];
    } *msg;
    uint32_t msg_pos;
    uint32_t msg_len;
    bool has_error;
    bool opened;
    bool flow_stopped;
    uint8_t guest_capabilities[MAX_GUEST_CAPABILITIES_BYTES];
    red::shared_ptr<StreamChannel> stream_channel;
    red::shared_ptr<CursorChannel> cursor_channel;
    SpiceTimer *close_timer;
    uint32_t frame_mmtime;
    StreamDeviceDisplayInfo device_display_info;

private:
    virtual RedPipeItemPtr read_one_msg_from_device() override;
    virtual void remove_client(RedCharDeviceClientOpaque *client) override;
    virtual void port_event(uint8_t event) override;

    bool partial_read();
    bool handle_msg_invalid(const char *error_msg) SPICE_GNUC_WARN_UNUSED_RESULT;
    bool handle_msg_capabilities() SPICE_GNUC_WARN_UNUSED_RESULT;
    bool handle_msg_format() SPICE_GNUC_WARN_UNUSED_RESULT;
    bool handle_msg_cursor_move() SPICE_GNUC_WARN_UNUSED_RESULT;
    bool handle_msg_cursor_set() SPICE_GNUC_WARN_UNUSED_RESULT;
    bool handle_msg_data() SPICE_GNUC_WARN_UNUSED_RESULT;
    bool handle_msg_device_display_info() SPICE_GNUC_WARN_UNUSED_RESULT;
    void reset_channels();
    static void close_timer_func(StreamDevice *dev);
    static void stream_start(void *opaque, StreamMsgStartStop *start,
                             StreamChannel *stream_channel);
    static void stream_queue_stat(void *opaque, const StreamQueueStat *stats,
                                  StreamChannel *stream_channel);
};

#include "pop-visibility.h"

#endif /* STREAM_DEVICE_H */
