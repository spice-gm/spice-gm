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

#include "red-channel-client.h"
#include "inputs-channel.h"

G_BEGIN_DECLS

class InputsChannelClient final: public RedChannelClient
{
    // TODO: RECEIVE_BUF_SIZE used to be the same for inputs_channel and main_channel
    // since it was defined once in reds.c which contained both.
    // Now that they are split we can give a more fitting value for inputs - what
    // should it be?
    enum {
        AGENT_WINDOW_SIZE = 10,
        NUM_INTERNAL_AGENT_MESSAGES = 1,

        // approximate max receive message size
        RECEIVE_BUF_SIZE =
            (4096 + (AGENT_WINDOW_SIZE + NUM_INTERNAL_AGENT_MESSAGES) *
                     SPICE_AGENT_MAX_DATA_SIZE)
    };

    using RedChannelClient::RedChannelClient;

    uint8_t recv_buf[RECEIVE_BUF_SIZE];
    virtual bool handle_message(uint16_t type, uint32_t size, void *message) override;
    uint16_t motion_count;
public:

    void send_migrate_data(SpiceMarshaller *m, RedPipeItem *item);
    void on_mouse_motion();
    void handle_migrate_data(uint16_t motion_count);

protected:
    virtual uint8_t *alloc_recv_buf(uint16_t type, uint32_t size) override;
    virtual void release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg) override;
    virtual void on_disconnect() override;
    virtual void send_item(RedPipeItem *base) override;
    virtual bool handle_migrate_data(uint32_t size, void *message) override;
    virtual void migrate() override;
};

RedChannelClient* inputs_channel_client_create(RedChannel *channel,
                                               RedClient *client,
                                               RedStream *stream,
                                               RedChannelCapabilities *caps);

G_END_DECLS

enum {
    RED_PIPE_ITEM_INPUTS_INIT = RED_PIPE_ITEM_TYPE_CHANNEL_BASE,
    RED_PIPE_ITEM_MOUSE_MOTION_ACK,
    RED_PIPE_ITEM_KEY_MODIFIERS,
    RED_PIPE_ITEM_MIGRATE_DATA,
};

#endif /* INPUTS_CHANNEL_CLIENT_H_ */
