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

#ifndef COMMON_GRAPHICS_CHANNEL_H_
#define COMMON_GRAPHICS_CHANNEL_H_

#include "red-channel.h"
#include "red-channel-client.h"

#include "push-visibility.h"

#define COMMON_CLIENT_TIMEOUT (NSEC_PER_SEC * 30)

class CommonGraphicsChannel: public RedChannel
{
public: // XXX
    int during_target_migrate = 0; /* TRUE when the client that is associated with the channel
                                  is during migration. Turned off when the vm is started.
                                  The flag is used to avoid sending messages that are artifacts
                                  of the transition from stopped vm to loaded vm (e.g., recreation
                                  of the primary surface) */
    using RedChannel::RedChannel;
};

void common_graphics_channel_set_during_target_migrate(CommonGraphicsChannel *self, gboolean value);
gboolean common_graphics_channel_get_during_target_migrate(CommonGraphicsChannel *self);

enum {
    RED_PIPE_ITEM_TYPE_INVAL_ONE = RED_PIPE_ITEM_TYPE_CHANNEL_BASE,

    RED_PIPE_ITEM_TYPE_COMMON_LAST
};

class CommonGraphicsChannelClient: public RedChannelClient
{
    enum { CHANNEL_RECEIVE_BUF_SIZE = 1024 };
    uint8_t recv_buf[CHANNEL_RECEIVE_BUF_SIZE];

public:
    using RedChannelClient::RedChannelClient;

protected:
    virtual uint8_t *alloc_recv_buf(uint16_t type, uint32_t size) override;
    virtual void release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg) override;
    virtual bool config_socket() override;
};

#include "pop-visibility.h"

#endif /* COMMON_GRAPHICS_CHANNEL_H_ */
