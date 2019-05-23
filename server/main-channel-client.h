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

#ifndef MAIN_CHANNEL_CLIENT_H_
#define MAIN_CHANNEL_CLIENT_H_

#include <common/messages.h>

#include "red-channel-client.h"
#include "main-channel.h"
#include "utils.hpp"

G_BEGIN_DECLS

struct MainChannelClientPrivate;

class MainChannelClient final: public RedChannelClient
{
public:
    MainChannelClient(MainChannel *channel,
                      RedClient *client,
                      RedStream *stream,
                      RedChannelCapabilities *caps,
                      uint32_t connection_id);

protected:
    virtual uint8_t *alloc_recv_buf(uint16_t type, uint32_t size) override;
    virtual void release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg) override;
    virtual void on_disconnect() override;
    virtual bool handle_message(uint16_t type, uint32_t size, void *message) override;

public:
    red::unique_link<MainChannelClientPrivate> priv;
};

MainChannelClient *main_channel_client_create(MainChannel *main_chan, RedClient *client,
                                              RedStream *stream, uint32_t connection_id,
                                              RedChannelCapabilities *caps);

void main_channel_client_push_agent_tokens(MainChannelClient *mcc, uint32_t num_tokens);
void main_channel_client_push_agent_data(MainChannelClient *mcc, uint8_t* data, size_t len,
                                         spice_marshaller_item_free_func free_data, void *opaque);
void main_channel_client_start_net_test(MainChannelClient *mcc, int test_rate);
// TODO: huge. Consider making a reds_* interface for these functions
// and calling from main.
void main_channel_client_push_init(MainChannelClient *mcc,
                                   int display_channels_hint,
                                   SpiceMouseMode current_mouse_mode,
                                   int is_client_mouse_allowed,
                                   int multi_media_time,
                                   int ram_hint);
void main_channel_client_push_notify(MainChannelClient *mcc, const char *msg);
void main_channel_client_migrate(RedChannelClient *rcc);
gboolean main_channel_client_connect_semi_seamless(MainChannelClient *mcc);
void main_channel_client_connect_seamless(MainChannelClient *mcc);
void main_channel_client_handle_migrate_connected(MainChannelClient *mcc,
                                                  int success,
                                                  int seamless);
void main_channel_client_handle_migrate_dst_do_seamless(MainChannelClient *mcc,
                                                        uint32_t src_version);
void main_channel_client_handle_migrate_end(MainChannelClient *mcc);
void main_channel_client_migrate_cancel_wait(MainChannelClient *mcc);
void main_channel_client_migrate_dst_complete(MainChannelClient *mcc);
gboolean main_channel_client_migrate_src_complete(MainChannelClient *mcc,
                                                  gboolean success);

void main_channel_client_handle_pong(MainChannelClient *mcc, SpiceMsgPing *ping, uint32_t size);

/*
 * return TRUE if network test had been completed successfully.
 * If FALSE, bitrate_per_sec is set to MAX_UINT64 and the roundtrip is set to 0
 */
int main_channel_client_is_network_info_initialized(MainChannelClient *mcc);
int main_channel_client_is_low_bandwidth(MainChannelClient *mcc);
uint64_t main_channel_client_get_bitrate_per_sec(MainChannelClient *mcc);
uint64_t main_channel_client_get_roundtrip_ms(MainChannelClient *mcc);

void main_channel_client_push_name(MainChannelClient *mcc, const char *name);
void main_channel_client_push_uuid(MainChannelClient *mcc, const uint8_t uuid[16]);

uint32_t main_channel_client_get_connection_id(MainChannelClient *mcc);
void main_channel_client_send_item(RedChannelClient *rcc, RedPipeItem *base);

enum {
    RED_PIPE_ITEM_TYPE_MAIN_CHANNELS_LIST = RED_PIPE_ITEM_TYPE_CHANNEL_BASE,
    RED_PIPE_ITEM_TYPE_MAIN_PING,
    RED_PIPE_ITEM_TYPE_MAIN_MOUSE_MODE,
    RED_PIPE_ITEM_TYPE_MAIN_AGENT_DISCONNECTED,
    RED_PIPE_ITEM_TYPE_MAIN_AGENT_TOKEN,
    RED_PIPE_ITEM_TYPE_MAIN_AGENT_DATA,
    RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_DATA,
    RED_PIPE_ITEM_TYPE_MAIN_INIT,
    RED_PIPE_ITEM_TYPE_MAIN_NOTIFY,
    RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN,
    RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN_SEAMLESS,
    RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_SWITCH_HOST,
    RED_PIPE_ITEM_TYPE_MAIN_MULTI_MEDIA_TIME,
    RED_PIPE_ITEM_TYPE_MAIN_NAME,
    RED_PIPE_ITEM_TYPE_MAIN_UUID,
    RED_PIPE_ITEM_TYPE_MAIN_AGENT_CONNECTED_TOKENS,
    RED_PIPE_ITEM_TYPE_MAIN_REGISTERED_CHANNEL,
};

RedPipeItem *main_mouse_mode_item_new(SpiceMouseMode current_mode, int is_client_mouse_allowed);

RedPipeItem *main_multi_media_time_item_new(uint32_t mm_time);

RedPipeItem *registered_channel_item_new(RedChannel *channel);

G_END_DECLS

#endif /* MAIN_CHANNEL_CLIENT_H_ */
