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
#include <config.h>

#include <common/generated_server_marshallers.h>

#include "common-graphics-channel.h"
#include "red-channel-client.h"
#include "cache-item.h"
#include "cursor-channel.h"
#include "cursor-channel-client.h"

#define CLIENT_CURSOR_CACHE_SIZE 256

#define CURSOR_CACHE_HASH_SHIFT 8
#define CURSOR_CACHE_HASH_SIZE (1 << CURSOR_CACHE_HASH_SHIFT)
#define CURSOR_CACHE_HASH_MASK (CURSOR_CACHE_HASH_SIZE - 1)
#define CURSOR_CACHE_HASH_KEY(id) ((id) & CURSOR_CACHE_HASH_MASK)
#define CURSOR_CLIENT_TIMEOUT 30000000000ULL //nano

struct CursorChannelClientPrivate
{
    SPICE_CXX_GLIB_ALLOCATOR

    RedCacheItem *cursor_cache[CURSOR_CACHE_HASH_SIZE];
    Ring cursor_cache_lru = { nullptr, nullptr };
    long cursor_cache_available = 0;
};

#define CLIENT_CURSOR_CACHE
#include "cache-item.tmpl.cpp"
#undef CLIENT_CURSOR_CACHE

#ifdef DEBUG_CURSORS
static int _cursor_count = 0;
#endif

void CursorChannelClient::reset_cursor_cache()
{
    red_cursor_cache_reset(this, CLIENT_CURSOR_CACHE_SIZE);
}

void CursorChannelClient::on_disconnect()
{
    reset_cursor_cache();
}

void CursorChannelClient::migrate()
{
    pipe_add_type(RED_PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE);
    RedChannelClient::migrate();
}

CursorChannelClient::CursorChannelClient(RedChannel *channel,
                                         RedClient *client,
                                         RedStream *stream,
                                         RedChannelCapabilities *caps):
    CommonGraphicsChannelClient(channel, client, stream, caps)
{
    ring_init(&priv->cursor_cache_lru);
    priv->cursor_cache_available = CLIENT_CURSOR_CACHE_SIZE;
}

CursorChannelClient* cursor_channel_client_new(CursorChannel *cursor, RedClient *client, RedStream *stream,
                                               int mig_target,
                                               RedChannelCapabilities *caps)
{
    auto rcc =
        red::make_shared<CursorChannelClient>(cursor, client, stream, caps);

    if (!rcc->init()) {
        return nullptr;
    }
    cursor->set_during_target_migrate(mig_target);

    return rcc.get();
}

RedCacheItem* CursorChannelClient::cache_find(uint64_t id)
{
    return red_cursor_cache_find(this, id);
}

int CursorChannelClient::cache_add(uint64_t id, size_t size)
{
    return red_cursor_cache_add(this, id, size);
}
