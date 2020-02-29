/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2016 Red Hat, Inc.

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

#ifndef CURSOR_CHANNEL_CLIENT_H_
#define CURSOR_CHANNEL_CLIENT_H_

#include "cache-item.h"
#include "red-common.h"
#include "red-channel-client.h"
#include "red-stream.h"
#include "cursor-channel.h"
#include "utils.hpp"

#include "push-visibility.h"

struct CursorChannelClientPrivate;

class CursorChannelClient final: public CommonGraphicsChannelClient
{
public:
    CursorChannelClient(RedChannel *channel,
                        RedClient *client,
                        RedStream *stream,
                        RedChannelCapabilities *caps);
    void reset_cursor_cache();
    RedCacheItem* cache_find(uint64_t id);
    int cache_add(uint64_t id, size_t size);
    CursorChannel* get_channel()
    {
        return static_cast<CursorChannel*>(CommonGraphicsChannelClient::get_channel());
    }

protected:
    virtual void on_disconnect() override;
    void send_item(RedPipeItem *pipe_item) override;
    /**
     * Migrate a client channel from a CursorChannel.
     * This is the equivalent of RedChannel client migrate callback.
     */
    virtual void migrate() override;

public:
    red::unique_link<CursorChannelClientPrivate> priv;
};

CursorChannelClient* cursor_channel_client_new(CursorChannel *cursor,
                                               RedClient *client,
                                               RedStream *stream,
                                               int mig_target,
                                               RedChannelCapabilities *caps);

enum {
    RED_PIPE_ITEM_TYPE_CURSOR = RED_PIPE_ITEM_TYPE_COMMON_LAST,
    RED_PIPE_ITEM_TYPE_CURSOR_INIT,
    RED_PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE,
};

#include "pop-visibility.h"

#endif /* CURSOR_CHANNEL_CLIENT_H_ */
