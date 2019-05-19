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
#include <config.h>

#include "inputs-channel-client.h"
#include "migration-protocol.h"
#include "red-channel-client.h"

XXX_CAST(RedChannelClient, InputsChannelClient, INPUTS_CHANNEL_CLIENT);

uint8_t *InputsChannelClient::alloc_recv_buf(uint16_t type, uint32_t size)
{
    if (size > sizeof(recv_buf)) {
        red_channel_warning(get_channel(), "error: too large incoming message");
        return NULL;
    }

    return recv_buf;
}

void InputsChannelClient::release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg)
{
}

void InputsChannelClient::on_disconnect()
{
    inputs_release_keys(INPUTS_CHANNEL(get_channel()));
}

RedChannelClient* inputs_channel_client_create(RedChannel *channel,
                                               RedClient *client,
                                               RedStream *stream,
                                               RedChannelCapabilities *caps)
{
    auto rcc = new InputsChannelClient(channel, client, stream, caps);
    if (!rcc->init()) {
        delete rcc;
        rcc = nullptr;
    }
    return rcc;
}

void inputs_channel_client_send_migrate_data(RedChannelClient *rcc,
                                             SpiceMarshaller *m,
                                             RedPipeItem *item)
{
    InputsChannelClient *icc = INPUTS_CHANNEL_CLIENT(rcc);

    rcc->init_send_data(SPICE_MSG_MIGRATE_DATA);

    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_INPUTS_MAGIC);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_INPUTS_VERSION);
    spice_marshaller_add_uint16(m, icc->motion_count);
}

void inputs_channel_client_handle_migrate_data(InputsChannelClient *icc,
                                               uint16_t motion_count)
{
    icc->motion_count = motion_count;

    for (; icc->motion_count >= SPICE_INPUT_MOTION_ACK_BUNCH;
           icc->motion_count -= SPICE_INPUT_MOTION_ACK_BUNCH) {
        icc->pipe_add_type(RED_PIPE_ITEM_MOUSE_MOTION_ACK);
    }
}

void inputs_channel_client_on_mouse_motion(InputsChannelClient *icc)
{
    InputsChannel *inputs_channel = INPUTS_CHANNEL(icc->get_channel());

    if (++icc->motion_count % SPICE_INPUT_MOTION_ACK_BUNCH == 0 &&
        !inputs_channel_is_src_during_migrate(inputs_channel)) {
        icc->pipe_add_type(RED_PIPE_ITEM_MOUSE_MOTION_ACK);
        icc->motion_count = 0;
    }
}
