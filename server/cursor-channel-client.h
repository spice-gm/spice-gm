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

#include <glib-object.h>

#include "cache-item.h"
#include "red-common.h"
#include "red-channel-client.h"
#include "red-stream.h"
#include "cursor-channel.h"

G_BEGIN_DECLS

#define TYPE_CURSOR_CHANNEL_CLIENT cursor_channel_client_get_type()

#define CURSOR_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_CURSOR_CHANNEL_CLIENT, CursorChannelClient))
#define CURSOR_CHANNEL_CLIENT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), TYPE_CURSOR_CHANNEL_CLIENT, CursorChannelClientClass))
#define IS_CURSOR_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), TYPE_CURSOR_CHANNEL_CLIENT))
#define IS_CURSOR_CHANNEL_CLIENT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass), TYPE_CURSOR_CHANNEL_CLIENT))
#define CURSOR_CHANNEL_CLIENT_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), TYPE_CURSOR_CHANNEL_CLIENT, CursorChannelClientClass))

typedef struct CursorChannelClient CursorChannelClient;
typedef struct CursorChannelClientClass CursorChannelClientClass;
typedef struct CursorChannelClientPrivate CursorChannelClientPrivate;

struct CursorChannelClient {
    CommonGraphicsChannelClient parent;

    CursorChannelClientPrivate *priv;
};

struct CursorChannelClientClass
{
    RedChannelClientClass parent_class;
};

GType cursor_channel_client_get_type(void) G_GNUC_CONST;

CursorChannelClient* cursor_channel_client_new(CursorChannel *cursor,
                                               RedClient *client,
                                               RedStream *stream,
                                               int mig_target,
                                               RedChannelCapabilities *caps);

void cursor_channel_client_reset_cursor_cache(RedChannelClient *rcc);
RedCacheItem* cursor_channel_client_cache_find(CursorChannelClient *ccc, uint64_t id);
int cursor_channel_client_cache_add(CursorChannelClient *ccc, uint64_t id, size_t size);

enum {
    RED_PIPE_ITEM_TYPE_CURSOR = RED_PIPE_ITEM_TYPE_COMMON_LAST,
    RED_PIPE_ITEM_TYPE_CURSOR_INIT,
    RED_PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE,
};

/**
 * Migrate a client channel from a CursorChannel.
 * This is the equivalent of RedChannel client migrate callback.
 */
void                 cursor_channel_client_migrate(RedChannelClient *client);

G_END_DECLS

#endif /* CURSOR_CHANNEL_CLIENT_H_ */
