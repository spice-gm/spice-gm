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

#include "push-visibility.h"

class MainChannelClientPrivate;

MainChannelClient *main_channel_client_create(MainChannel *main_chan, RedClient *client,
                                              RedStream *stream, uint32_t connection_id,
                                              RedChannelCapabilities *caps);

struct RedAgentDataPipeItem;

class MainChannelClient final: public RedChannelClient
{
public:
    void push_agent_tokens(uint32_t num_tokens);
    void push_agent_data(red::shared_ptr<RedAgentDataPipeItem>&& item);
    // TODO: huge. Consider making a reds_* interface for these functions
    // and calling from main.
    void push_init(int display_channels_hint, SpiceMouseMode current_mouse_mode,
                   int is_client_mouse_allowed, int multi_media_time,
                   int ram_hint);
    void push_notify(const char *msg);
    gboolean connect_semi_seamless();
    void connect_seamless();
    void handle_migrate_connected(int success, int seamless);
    void handle_migrate_dst_do_seamless(uint32_t src_version);
    void handle_migrate_end();
    void migrate_cancel_wait();
    void migrate_dst_complete();
    gboolean migrate_src_complete(gboolean success);

    /*
     * return TRUE if network test had been completed successfully.
     * If FALSE, bitrate_per_sec is set to MAX_UINT64 and the roundtrip is set to 0
     */
    bool is_network_info_initialized() const;
    bool is_low_bandwidth() const;
    uint64_t get_bitrate_per_sec() const;
    uint64_t get_roundtrip_ms() const;

    void push_name(const char *name);
    void push_uuid(const uint8_t uuid[16]);

    uint32_t get_connection_id() const;

    MainChannelClient(MainChannel *channel,
                      RedClient *client,
                      RedStream *stream,
                      RedChannelCapabilities *caps,
                      uint32_t connection_id);

    void handle_pong(SpiceMsgPing *ping, uint32_t size);
    void start_net_test(int test_rate);
    MainChannel* get_channel()
    {
        return static_cast<MainChannel*>(RedChannelClient::get_channel());
    }

protected:
    virtual uint8_t *alloc_recv_buf(uint16_t type, uint32_t size) override;
    virtual void release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg) override;
    virtual void on_disconnect() override;
    virtual bool handle_message(uint16_t type, uint32_t size, void *message) override;
    virtual void send_item(RedPipeItem *item)  override;
    virtual bool handle_migrate_data(uint32_t size, void *message) override;
    virtual void migrate() override;
    virtual void handle_migrate_flush_mark() override;

public:
    red::unique_link<MainChannelClientPrivate> priv;
};

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

struct RedAgentDataPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MAIN_AGENT_DATA> {
    int len = 0;
    uint8_t data[SPICE_AGENT_MAX_DATA_SIZE];
};

RedPipeItemPtr main_mouse_mode_item_new(SpiceMouseMode current_mode, int is_client_mouse_allowed);

RedPipeItemPtr main_multi_media_time_item_new(uint32_t mm_time);

RedPipeItemPtr registered_channel_item_new(RedChannel *channel);

#include "pop-visibility.h"

#endif /* MAIN_CHANNEL_CLIENT_H_ */
