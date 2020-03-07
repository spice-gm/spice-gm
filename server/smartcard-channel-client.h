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

#ifndef SMARTCARD_CHANNEL_CLIENT_H_
#define SMARTCARD_CHANNEL_CLIENT_H_

#include "smartcard.h"
#include "utils.hpp"

#include "push-visibility.h"

struct SmartCardChannelClientPrivate;

class SmartCardChannelClient final: public RedChannelClient
{
protected:
    ~SmartCardChannelClient();
public:
    red::unique_link<SmartCardChannelClientPrivate> priv;
    SmartCardChannelClient(RedChannel *channel,
                           RedClient *client,
                           RedStream *stream,
                           RedChannelCapabilities *caps);

private:
    virtual uint8_t *alloc_recv_buf(uint16_t type, uint32_t size) override;
    virtual void release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg) override;
    virtual void on_disconnect() override;
    virtual bool handle_message(uint16_t type, uint32_t size, void *message) override;
    virtual void send_item(RedPipeItem *item) override;
    virtual bool handle_migrate_data(uint32_t size, void *message) override;
    virtual void handle_migrate_flush_mark() override;
};

SmartCardChannelClient* smartcard_channel_client_create(RedChannel *channel,
                                                        RedClient *client, RedStream *stream,
                                                        RedChannelCapabilities *caps);

void smartcard_channel_client_send_data(RedChannelClient *rcc,
                                        SpiceMarshaller *m,
                                        RedPipeItem *item,
                                        VSCMsgHeader *vheader);

void smartcard_channel_client_send_error(RedChannelClient *rcc,
                                         SpiceMarshaller *m,
                                         RedPipeItem *item);

void smartcard_channel_client_set_char_device(SmartCardChannelClient *scc,
                                              RedCharDeviceSmartcard *device);

red::shared_ptr<RedCharDeviceSmartcard>
smartcard_channel_client_get_char_device(SmartCardChannelClient *scc);

#include "pop-visibility.h"

#endif /* SMARTCARD_CHANNEL_CLIENT_H_ */
