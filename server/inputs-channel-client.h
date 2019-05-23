/*
   Copyright (C) 2009-2015 Red Hat, Inc.

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

#ifndef INPUTS_CHANNEL_CLIENT_H_
#define INPUTS_CHANNEL_CLIENT_H_

#include <glib-object.h>

#include "red-channel-client.h"
#include "inputs-channel.h"

G_BEGIN_DECLS

#define TYPE_INPUTS_CHANNEL_CLIENT inputs_channel_client_get_type()

#define INPUTS_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_INPUTS_CHANNEL_CLIENT, InputsChannelClient))
#define INPUTS_CHANNEL_CLIENT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), TYPE_INPUTS_CHANNEL_CLIENT, InputsChannelClientClass))
#define IS_INPUTS_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), TYPE_INPUTS_CHANNEL_CLIENT))
#define IS_INPUTS_CHANNEL_CLIENT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass), TYPE_INPUTS_CHANNEL_CLIENT))
#define INPUTS_CHANNEL_CLIENT_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), TYPE_INPUTS_CHANNEL_CLIENT, InputsChannelClientClass))

typedef struct InputsChannelClient InputsChannelClient;
typedef struct InputsChannelClientClass InputsChannelClientClass;
typedef struct InputsChannelClientPrivate InputsChannelClientPrivate;

struct InputsChannelClient
{
    RedChannelClient parent;

    InputsChannelClientPrivate *priv;
};

struct InputsChannelClientClass
{
    RedChannelClientClass parent_class;
};

GType inputs_channel_client_get_type(void) G_GNUC_CONST;

RedChannelClient* inputs_channel_client_create(RedChannel *channel,
                                               RedClient *client,
                                               RedStream *stream,
                                               RedChannelCapabilities *caps);

void inputs_channel_client_on_mouse_motion(InputsChannelClient* self);
void inputs_channel_client_send_migrate_data(RedChannelClient *rcc,
                                             SpiceMarshaller *m, RedPipeItem *item);
void inputs_channel_client_handle_migrate_data(InputsChannelClient *icc, uint16_t motion_count);

G_END_DECLS

enum {
    RED_PIPE_ITEM_INPUTS_INIT = RED_PIPE_ITEM_TYPE_CHANNEL_BASE,
    RED_PIPE_ITEM_MOUSE_MOTION_ACK,
    RED_PIPE_ITEM_KEY_MODIFIERS,
    RED_PIPE_ITEM_MIGRATE_DATA,
};

#endif /* INPUTS_CHANNEL_CLIENT_H_ */
