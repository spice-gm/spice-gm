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

#include <inttypes.h>
#include <common/generated_server_marshallers.h>

#include "main-channel-client.h"
#include "main-channel.h"
#include "red-channel-client.h"
#include "red-client.h"
#include "reds.h"

#define NET_TEST_WARMUP_BYTES 0
#define NET_TEST_BYTES (1024 * 250)

enum NetTestStage {
    NET_TEST_STAGE_INVALID,
    NET_TEST_STAGE_WARMUP,
    NET_TEST_STAGE_LATENCY,
    NET_TEST_STAGE_RATE,
    NET_TEST_STAGE_COMPLETE,
};

#define CLIENT_CONNECTIVITY_TIMEOUT (MSEC_PER_SEC * 30)

// approximate max receive message size for main channel
#define MAIN_CHANNEL_RECEIVE_BUF_SIZE \
    (4096 + (REDS_AGENT_WINDOW_SIZE + REDS_NUM_INTERNAL_AGENT_MESSAGES) * SPICE_AGENT_MAX_DATA_SIZE)

struct MainChannelClientPrivate {
    SPICE_CXX_GLIB_ALLOCATOR

    uint32_t connection_id;
    uint32_t ping_id = 0;
    uint32_t net_test_id = 0;
    NetTestStage net_test_stage = NET_TEST_STAGE_INVALID;
    uint64_t latency = 0;
    uint64_t bitrate_per_sec = ~0;
    int mig_wait_connect = 0;
    int mig_connect_ok = 0;
    int mig_wait_prev_complete = 0;
    int mig_wait_prev_try_seamless = 0;
    int init_sent = 0;
    int seamless_mig_dst = 0;
    bool initial_channels_list_sent = false;
    uint8_t recv_buf[MAIN_CHANNEL_RECEIVE_BUF_SIZE];
};

struct RedPingPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MAIN_PING> {
    int size;
};

struct RedTokensPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MAIN_AGENT_TOKEN> {
    int tokens;
};

struct RedInitPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MAIN_INIT> {
    int connection_id;
    int display_channels_hint;
    int current_mouse_mode;
    int is_client_mouse_allowed;
    int multi_media_time;
    int ram_hint;
};

struct RedNamePipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MAIN_NAME> {
    SpiceMsgMainName msg;
};

struct RedUuidPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MAIN_UUID> {
    SpiceMsgMainUuid msg;
};

struct RedNotifyPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MAIN_NOTIFY> {
    red::glib_unique_ptr<char> msg;
};

struct RedMouseModePipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MAIN_MOUSE_MODE> {
    SpiceMouseMode current_mode;
    int is_client_mouse_allowed;
};

struct RedMultiMediaTimePipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MAIN_MULTI_MEDIA_TIME> {
    uint32_t time;
};

struct RedRegisteredChannelPipeItem:
    public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MAIN_REGISTERED_CHANNEL>
{
    uint32_t channel_type;
    uint32_t channel_id;
};

#define ZERO_BUF_SIZE 4096

static const uint8_t zero_page[ZERO_BUF_SIZE] = {0};

uint8_t *MainChannelClient::alloc_recv_buf(uint16_t type, uint32_t size)
{
    if (type == SPICE_MSGC_MAIN_AGENT_DATA) {
        RedChannel *channel = get_channel();
        return reds_get_agent_data_buffer(channel->get_server(), this, size);
    }

    if (size > sizeof(priv->recv_buf)) {
        /* message too large, caller will log a message and close the connection */
        return nullptr;
    }

    return priv->recv_buf;
}

void MainChannelClient::release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg)
{
    if (type == SPICE_MSGC_MAIN_AGENT_DATA) {
        RedChannel *channel = get_channel();
        reds_release_agent_data_buffer(channel->get_server(), msg);
    }
}

/*
 * When the main channel is disconnected, disconnect the entire client.
 */
void MainChannelClient::on_disconnect()
{
    RedsState *reds = get_channel()->get_server();
    reds_get_main_dispatcher(reds)->client_disconnect(get_client());
}

static void main_channel_client_push_ping(MainChannelClient *mcc, int size);

static RedPipeItemPtr main_notify_item_new(const char *msg, int num)
{
    auto item = red::make_shared<RedNotifyPipeItem>();

    item->msg.reset(g_strdup(msg));
    return item;
}

void MainChannelClient::start_net_test(int test_rate)
{
    if (priv->net_test_id) {
        return;
    }

    if (!test_rate) {
        start_connectivity_monitoring(CLIENT_CONNECTIVITY_TIMEOUT);
        return;
    }

    priv->net_test_id = priv->ping_id + 1;
    priv->net_test_stage = NET_TEST_STAGE_WARMUP;

    main_channel_client_push_ping(this, NET_TEST_WARMUP_BYTES);
    main_channel_client_push_ping(this, 0);
    main_channel_client_push_ping(this, NET_TEST_BYTES);
}

static RedPipeItemPtr red_ping_item_new(int size)
{
    auto item = red::make_shared<RedPingPipeItem>();

    item->size = size;
    return item;
}

static void main_channel_client_push_ping(MainChannelClient *mcc, int size)
{
    auto item = red_ping_item_new(size);
    mcc->pipe_add_push(std::move(item));
}

static RedPipeItemPtr main_agent_tokens_item_new(uint32_t num_tokens)
{
    auto item = red::make_shared<RedTokensPipeItem>();

    item->tokens = num_tokens;
    return item;
}


void MainChannelClient::push_agent_tokens(uint32_t num_tokens)
{
    auto item = main_agent_tokens_item_new(num_tokens);

    pipe_add_push(std::move(item));
}

void MainChannelClient::push_agent_data(red::shared_ptr<RedAgentDataPipeItem>&& item)
{
    pipe_add_push(item);
}

static RedPipeItemPtr
main_init_item_new(int connection_id,
                   int display_channels_hint,
                   SpiceMouseMode current_mouse_mode,
                   int is_client_mouse_allowed,
                   int multi_media_time,
                   int ram_hint)
{
    auto item = red::make_shared<RedInitPipeItem>();

    item->connection_id = connection_id;
    item->display_channels_hint = display_channels_hint;
    item->current_mouse_mode = current_mouse_mode;
    item->is_client_mouse_allowed = is_client_mouse_allowed;
    item->multi_media_time = multi_media_time;
    item->ram_hint = ram_hint;
    return item;
}

void MainChannelClient::push_init(int display_channels_hint,
                                  SpiceMouseMode current_mouse_mode,
                                  int is_client_mouse_allowed,
                                  int multi_media_time, int ram_hint)
{
    auto item = main_init_item_new(priv->connection_id, display_channels_hint,
                                   current_mouse_mode, is_client_mouse_allowed,
                                   multi_media_time, ram_hint);
    pipe_add_push(std::move(item));
}

static RedPipeItemPtr main_name_item_new(const char *name)
{
    auto item = new (strlen(name) + 1) RedNamePipeItem();
    item->msg.name_len = strlen(name) + 1;
    memcpy(&item->msg.name, name, item->msg.name_len);

    return RedPipeItemPtr(item);
}

void MainChannelClient::push_name(const char *name)
{
    if (!test_remote_cap(SPICE_MAIN_CAP_NAME_AND_UUID))
        return;

    auto item = main_name_item_new(name);
    pipe_add_push(std::move(item));
}

static RedPipeItemPtr main_uuid_item_new(const uint8_t uuid[16])
{
    auto item = red::make_shared<RedUuidPipeItem>();

    memcpy(item->msg.uuid, uuid, sizeof(item->msg.uuid));

    return item;
}

void MainChannelClient::push_uuid(const uint8_t uuid[16])
{
    if (!test_remote_cap(SPICE_MAIN_CAP_NAME_AND_UUID))
        return;

    auto item = main_uuid_item_new(uuid);
    pipe_add_push(std::move(item));
}

void MainChannelClient::push_notify(const char *msg)
{
    auto item = main_notify_item_new(msg, 1);
    pipe_add_push(std::move(item));
}

RedPipeItemPtr
main_mouse_mode_item_new(SpiceMouseMode current_mode, int is_client_mouse_allowed)
{
    auto item = red::make_shared<RedMouseModePipeItem>();

    item->current_mode = current_mode;
    item->is_client_mouse_allowed = is_client_mouse_allowed;
    return item;
}

RedPipeItemPtr
main_multi_media_time_item_new(uint32_t mm_time)
{
    auto item = red::make_shared<RedMultiMediaTimePipeItem>();
    item->time = mm_time;
    return item;
}

RedPipeItemPtr
registered_channel_item_new(RedChannel *channel)
{
    auto item = red::make_shared<RedRegisteredChannelPipeItem>();

    item->channel_type = channel->type();
    item->channel_id = channel->id();
    return item;
}

void MainChannelClient::handle_migrate_connected(int success, int seamless)
{
    if (priv->mig_wait_connect) {
        MainChannel *channel = get_channel();

        priv->mig_wait_connect = FALSE;
        priv->mig_connect_ok = success;
        channel->on_migrate_connected(success, seamless);
    } else {
        if (success) {
            pipe_add_empty_msg(SPICE_MSG_MAIN_MIGRATE_CANCEL);
        }
    }
}

void MainChannelClient::handle_migrate_dst_do_seamless(uint32_t src_version)
{
    RedChannel *channel = get_channel();
    if (reds_on_migrate_dst_set_seamless(channel->get_server(), this, src_version)) {
        priv->seamless_mig_dst = TRUE;
        pipe_add_empty_msg(SPICE_MSG_MAIN_MIGRATE_DST_SEAMLESS_ACK);
    } else {
        pipe_add_empty_msg(SPICE_MSG_MAIN_MIGRATE_DST_SEAMLESS_NACK);
    }
}

void MainChannelClient::handle_pong(SpiceMsgPing *ping, uint32_t size)
{
    uint64_t roundtrip;

    roundtrip = spice_get_monotonic_time_ns() / NSEC_PER_MICROSEC - ping->timestamp;

    if (ping->id != priv->net_test_id) {
        /*
         * channel client monitors the connectivity using ping-pong messages
         */
        RedChannelClient::handle_message(SPICE_MSGC_PONG, size, ping);
        return;
    }

    switch (priv->net_test_stage) {
    case NET_TEST_STAGE_WARMUP:
        priv->net_test_id++;
        priv->net_test_stage = NET_TEST_STAGE_LATENCY;
        priv->latency = roundtrip;
        break;
    case NET_TEST_STAGE_LATENCY:
        priv->net_test_id++;
        priv->net_test_stage = NET_TEST_STAGE_RATE;
        priv->latency = MIN(priv->latency, roundtrip);
        break;
    case NET_TEST_STAGE_RATE:
        priv->net_test_id = 0;
        if (roundtrip <= priv->latency) {
            // probably high load on client or server result with incorrect values
            red_channel_debug(get_channel(),
                              "net test: invalid values, latency %" G_GUINT64_FORMAT
                              " roundtrip %" G_GUINT64_FORMAT ". assuming high"
                              "bandwidth", priv->latency, roundtrip);
            priv->latency = 0;
            priv->net_test_stage = NET_TEST_STAGE_INVALID;
            start_connectivity_monitoring(CLIENT_CONNECTIVITY_TIMEOUT);
            break;
        }
        priv->bitrate_per_sec = (uint64_t)(NET_TEST_BYTES * 8) * 1000000
            / (roundtrip - priv->latency);
        priv->net_test_stage = NET_TEST_STAGE_COMPLETE;
        red_channel_debug(get_channel(),
                          "net test: latency %f ms, bitrate %" G_GUINT64_FORMAT " bps (%f Mbps)%s",
                          (double)priv->latency / 1000,
                          priv->bitrate_per_sec,
                          (double)priv->bitrate_per_sec / 1024 / 1024,
                          this->is_low_bandwidth() ? " LOW BANDWIDTH" : "");
        start_connectivity_monitoring(CLIENT_CONNECTIVITY_TIMEOUT);
        break;
    default:
        red_channel_warning(get_channel(),
                            "invalid net test stage, ping id %d test id %d stage %d",
                            ping->id,
                            priv->net_test_id,
                            priv->net_test_stage);
        priv->net_test_stage = NET_TEST_STAGE_INVALID;
    }
}

void MainChannelClient::handle_migrate_end()
{
    RedClient *client = get_client();
    if (!client->during_migrate_at_target()) {
        red_channel_warning(get_channel(),
                            "unexpected SPICE_MSGC_MIGRATE_END");
        return;
    }
    if (!test_remote_cap(SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE)) {
        red_channel_warning(get_channel(),
                            "unexpected SPICE_MSGC_MIGRATE_END, "
                            "client does not support semi-seamless migration");
        return;
    }
    client->semi_seamless_migrate_complete();
}

void MainChannelClient::migrate_cancel_wait()
{
    if (priv->mig_wait_connect) {
        priv->mig_wait_connect = FALSE;
        priv->mig_connect_ok = FALSE;
    }
    priv->mig_wait_prev_complete = FALSE;
}

void MainChannelClient::migrate_dst_complete()
{
    if (priv->mig_wait_prev_complete) {
        if (priv->mig_wait_prev_try_seamless) {
            RedChannel *channel = get_channel();
            spice_assert(channel->get_n_clients() == 1);
            pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN_SEAMLESS);
        } else {
            pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN);
        }
        priv->mig_wait_connect = TRUE;
        priv->mig_wait_prev_complete = FALSE;
    }
}

gboolean MainChannelClient::migrate_src_complete(gboolean success)
{
    gboolean ret = FALSE;
    bool semi_seamless_support = test_remote_cap(SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE);
    if (semi_seamless_support && priv->mig_connect_ok) {
        if (success) {
            pipe_add_empty_msg(SPICE_MSG_MAIN_MIGRATE_END);
            ret = TRUE;
        } else {
            pipe_add_empty_msg(SPICE_MSG_MAIN_MIGRATE_CANCEL);
        }
    } else {
        if (success) {
            pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_SWITCH_HOST);
        }
    }
    priv->mig_connect_ok = FALSE;
    priv->mig_wait_connect = FALSE;

    return ret;
}


MainChannelClient::MainChannelClient(MainChannel *channel,
                                     RedClient *client,
                                     RedStream *stream,
                                     RedChannelCapabilities *caps,
                                     uint32_t connection_id):
    RedChannelClient(channel, client, stream, caps)
{
    priv->connection_id = connection_id;
}

MainChannelClient *main_channel_client_create(MainChannel *main_chan, RedClient *client,
                                              RedStream *stream, uint32_t connection_id,
                                              RedChannelCapabilities *caps)
{
    auto mcc =
        red::make_shared<MainChannelClient>(main_chan, client, stream, caps, connection_id);
    if (!mcc->init()) {
        return nullptr;
    }
    return mcc.get();
}

bool MainChannelClient::is_network_info_initialized() const
{
    return priv->net_test_stage == NET_TEST_STAGE_COMPLETE;
}

bool MainChannelClient::is_low_bandwidth() const
{
    // TODO: configurable?
    return priv->bitrate_per_sec < 10 * 1024 * 1024;
}

uint64_t MainChannelClient::get_bitrate_per_sec() const
{
    return priv->bitrate_per_sec;
}

uint64_t MainChannelClient::get_roundtrip_ms() const
{
    return priv->latency / 1000;
}

void MainChannelClient::migrate()
{
    RedChannel *channel = get_channel();
    reds_on_main_channel_migrate(channel->get_server(), this);
    RedChannelClient::migrate();
}

gboolean MainChannelClient::connect_semi_seamless()
{
    if (test_remote_cap(SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE)) {
        RedClient *client = get_client();
        if (client->during_migrate_at_target()) {
            priv->mig_wait_prev_complete = TRUE;
            priv->mig_wait_prev_try_seamless = FALSE;
        } else {
            pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN);
            priv->mig_wait_connect = TRUE;
        }
        priv->mig_connect_ok = FALSE;
        return TRUE;
    }
    return FALSE;
}

void MainChannelClient::connect_seamless()
{
    RedClient *client = get_client();
    spice_assert(test_remote_cap(SPICE_MAIN_CAP_SEAMLESS_MIGRATE));
    if (client->during_migrate_at_target()) {
        priv->mig_wait_prev_complete = TRUE;
        priv->mig_wait_prev_try_seamless = TRUE;
    } else {
        pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN_SEAMLESS);
        priv->mig_wait_connect = TRUE;
    }
    priv->mig_connect_ok = FALSE;
}

uint32_t MainChannelClient::get_connection_id() const
{
    return priv->connection_id;
}

static uint32_t main_channel_client_next_ping_id(MainChannelClient *mcc)
{
    return ++mcc->priv->ping_id;
}

static void main_channel_marshall_channels(RedChannelClient *rcc,
                                           SpiceMarshaller *m,
                                           RedPipeItem *item)
{
    SpiceMsgChannels* channels_info;
    RedChannel *channel = rcc->get_channel();

    rcc->init_send_data(SPICE_MSG_MAIN_CHANNELS_LIST);
    channels_info = reds_msg_channels_new(channel->get_server());
    spice_marshall_msg_main_channels_list(m, channels_info);
    g_free(channels_info);
}

static void main_channel_marshall_ping(MainChannelClient *mcc,
                                       SpiceMarshaller *m,
                                       RedPingPipeItem *item)
{
    SpiceMsgPing ping;
    int size_left = item->size;

    mcc->init_send_data(SPICE_MSG_PING);
    ping.id = main_channel_client_next_ping_id(mcc);
    ping.timestamp = spice_get_monotonic_time_ns() / NSEC_PER_MICROSEC;
    spice_marshall_msg_ping(m, &ping);

    while (size_left > 0) {
        int now = MIN(ZERO_BUF_SIZE, size_left);
        size_left -= now;
        spice_marshaller_add_by_ref(m, zero_page, now);
    }
}

static void main_channel_marshall_mouse_mode(RedChannelClient *rcc,
                                             SpiceMarshaller *m,
                                             RedMouseModePipeItem *item)
{
    SpiceMsgMainMouseMode mouse_mode;

    rcc->init_send_data(SPICE_MSG_MAIN_MOUSE_MODE);
    mouse_mode.supported_modes = SPICE_MOUSE_MODE_SERVER;
    if (item->is_client_mouse_allowed) {
        mouse_mode.supported_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    mouse_mode.current_mode = item->current_mode;
    spice_marshall_msg_main_mouse_mode(m, &mouse_mode);
}

static void main_channel_marshall_agent_disconnected(RedChannelClient *rcc,
                                                     SpiceMarshaller *m,
                                                     RedPipeItem *item)
{
    SpiceMsgMainAgentDisconnect disconnect;

    rcc->init_send_data(SPICE_MSG_MAIN_AGENT_DISCONNECTED);
    disconnect.error_code = SPICE_LINK_ERR_OK;
    spice_marshall_msg_main_agent_disconnected(m, &disconnect);
}

static void main_channel_marshall_tokens(RedChannelClient *rcc,
                                         SpiceMarshaller *m, RedTokensPipeItem *item)
{
    SpiceMsgMainAgentTokens tokens;

    rcc->init_send_data(SPICE_MSG_MAIN_AGENT_TOKEN);
    tokens.num_tokens = item->tokens;
    spice_marshall_msg_main_agent_token(m, &tokens);
}

static void main_channel_marshall_agent_data(RedChannelClient *rcc,
                                             SpiceMarshaller *m,
                                             RedAgentDataPipeItem *item)
{
    rcc->init_send_data(SPICE_MSG_MAIN_AGENT_DATA);
    /* since pipe item owns the data, keep it alive until it's sent */
    item->add_to_marshaller(m, item->data, item->len);
}

static void main_channel_marshall_migrate_data_item(RedChannelClient *rcc,
                                                    SpiceMarshaller *m,
                                                    RedPipeItem *item)
{
    RedChannel *channel = rcc->get_channel();
    rcc->init_send_data(SPICE_MSG_MIGRATE_DATA);
    // TODO: from reds split. ugly separation.
    reds_marshall_migrate_data(channel->get_server(), m);
}

static void main_channel_marshall_init(RedChannelClient *rcc,
                                       SpiceMarshaller *m,
                                       RedInitPipeItem *item)
{
    SpiceMsgMainInit init; // TODO - remove this copy, make RedInitPipeItem reuse SpiceMsgMainInit
    RedChannel *channel = rcc->get_channel();

    rcc->init_send_data(SPICE_MSG_MAIN_INIT);
    init.session_id = item->connection_id;
    init.display_channels_hint = item->display_channels_hint;
    init.current_mouse_mode = item->current_mouse_mode;
    init.supported_mouse_modes = SPICE_MOUSE_MODE_SERVER;
    if (item->is_client_mouse_allowed) {
        init.supported_mouse_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    init.agent_connected = reds_has_vdagent(channel->get_server());
    init.agent_tokens = REDS_AGENT_WINDOW_SIZE;
    init.multi_media_time = item->multi_media_time;
    init.ram_hint = item->ram_hint;
    spice_marshall_msg_main_init(m, &init);
}

static void main_channel_marshall_notify(RedChannelClient *rcc,
                                         SpiceMarshaller *m, RedNotifyPipeItem *item)
{
    SpiceMsgNotify notify;

    rcc->init_send_data(SPICE_MSG_NOTIFY);
    notify.time_stamp = spice_get_monotonic_time_ns(); // TODO - move to main_new_notify_item
    notify.severity = SPICE_NOTIFY_SEVERITY_WARN;
    notify.visibilty = SPICE_NOTIFY_VISIBILITY_HIGH;
    notify.what = SPICE_WARN_GENERAL;
    notify.message_len = strlen(item->msg.get());
    spice_marshall_msg_notify(m, &notify);
    spice_marshaller_add(m, (uint8_t *)item->msg.get(), notify.message_len + 1);
}

static void main_channel_fill_migrate_dst_info(MainChannel *main_channel,
                                               SpiceMigrationDstInfo *dst_info)
{
    const RedsMigSpice *mig_dst = main_channel->get_migration_target();
    dst_info->port = mig_dst->port;
    dst_info->sport = mig_dst->sport;
    dst_info->host_size = strlen(mig_dst->host) + 1;
    dst_info->host_data = (uint8_t *)mig_dst->host;
    if (mig_dst->cert_subject) {
        dst_info->cert_subject_size = strlen(mig_dst->cert_subject) + 1;
        dst_info->cert_subject_data = (uint8_t *)mig_dst->cert_subject;
    } else {
        dst_info->cert_subject_size = 0;
        dst_info->cert_subject_data = nullptr;
    }
}

static void main_channel_marshall_migrate_begin(SpiceMarshaller *m, MainChannelClient *rcc,
                                                RedPipeItem *item)
{
    MainChannel *channel = rcc->get_channel();
    SpiceMsgMainMigrationBegin migrate;

    rcc->init_send_data(SPICE_MSG_MAIN_MIGRATE_BEGIN);
    main_channel_fill_migrate_dst_info(channel, &migrate.dst_info);
    spice_marshall_msg_main_migrate_begin(m, &migrate);
}

static void main_channel_marshall_migrate_begin_seamless(SpiceMarshaller *m,
                                                         MainChannelClient *rcc,
                                                         RedPipeItem *item)
{
    MainChannel *channel = rcc->get_channel();
    SpiceMsgMainMigrateBeginSeamless migrate_seamless;

    rcc->init_send_data(SPICE_MSG_MAIN_MIGRATE_BEGIN_SEAMLESS);
    main_channel_fill_migrate_dst_info(channel, &migrate_seamless.dst_info);
    migrate_seamless.src_mig_version = SPICE_MIGRATION_PROTOCOL_VERSION;
    spice_marshall_msg_main_migrate_begin_seamless(m, &migrate_seamless);
}

static void main_channel_marshall_multi_media_time(RedChannelClient *rcc,
                                                   SpiceMarshaller *m,
                                                   RedMultiMediaTimePipeItem *item)
{
    SpiceMsgMainMultiMediaTime time_mes;

    rcc->init_send_data(SPICE_MSG_MAIN_MULTI_MEDIA_TIME);
    time_mes.time = item->time;
    spice_marshall_msg_main_multi_media_time(m, &time_mes);
}

static void main_channel_marshall_migrate_switch(SpiceMarshaller *m, MainChannelClient *rcc,
                                                 RedPipeItem *item)
{
    MainChannel *channel = rcc->get_channel();
    SpiceMsgMainMigrationSwitchHost migrate;
    const RedsMigSpice *mig_target;

    rcc->init_send_data(SPICE_MSG_MAIN_MIGRATE_SWITCH_HOST);
    mig_target = channel->get_migration_target();
    migrate.port = mig_target->port;
    migrate.sport = mig_target->sport;
    migrate.host_size = strlen(mig_target->host) + 1;
    migrate.host_data = (uint8_t *)mig_target->host;
    if (mig_target->cert_subject) {
        migrate.cert_subject_size = strlen(mig_target->cert_subject) + 1;
        migrate.cert_subject_data = (uint8_t *)mig_target->cert_subject;
    } else {
        migrate.cert_subject_size = 0;
        migrate.cert_subject_data = nullptr;
    }
    spice_marshall_msg_main_migrate_switch_host(m, &migrate);
}

static void main_channel_marshall_agent_connected(SpiceMarshaller *m,
                                                  RedChannelClient *rcc,
                                                  RedPipeItem *item)
{
    SpiceMsgMainAgentConnectedTokens connected;

    rcc->init_send_data(SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS);
    connected.num_tokens = REDS_AGENT_WINDOW_SIZE;
    spice_marshall_msg_main_agent_connected_tokens(m, &connected);
}

static void main_channel_marshall_registered_channel(RedChannelClient *rcc,
                                                     SpiceMarshaller *m,
                                                     RedRegisteredChannelPipeItem *item)
{
    struct {
        SpiceMsgChannels info;
        SpiceChannelId ids[1];
    } channels_info_buffer;
    SpiceMsgChannels* channels_info = &channels_info_buffer.info;

    rcc->init_send_data(SPICE_MSG_MAIN_CHANNELS_LIST);

    channels_info->channels[0].type = item->channel_type;
    channels_info->channels[0].id = item->channel_id;
    channels_info->num_of_channels = 1;

    spice_marshall_msg_main_channels_list(m, channels_info);
}

void MainChannelClient::send_item(RedPipeItem *base)
{
    SpiceMarshaller *m = get_marshaller();

    /* In semi-seamless migration (dest side), the connection is started from scratch, and
     * we ignore any pipe item that arrives before the INIT msg is sent.
     * For seamless we don't send INIT, and the connection continues from the same place
     * it stopped on the src side. */
    if (!priv->init_sent &&
        !priv->seamless_mig_dst &&
        base->type != RED_PIPE_ITEM_TYPE_MAIN_INIT) {
        red_channel_warning(get_channel(),
                            "Init msg for client %p was not sent yet "
                            "(client is probably during semi-seamless migration). Ignoring msg type %d",
                            get_client(), base->type);
        return;
    }
    switch (base->type) {
        case RED_PIPE_ITEM_TYPE_MAIN_CHANNELS_LIST:
            main_channel_marshall_channels(this, m, base);
            priv->initial_channels_list_sent = true;
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_PING:
            main_channel_marshall_ping(this, m,
                static_cast<RedPingPipeItem*>(base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MOUSE_MODE:
            main_channel_marshall_mouse_mode(this, m,
                static_cast<RedMouseModePipeItem*>(base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_DISCONNECTED:
            main_channel_marshall_agent_disconnected(this, m, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_TOKEN:
            main_channel_marshall_tokens(this, m,
                static_cast<RedTokensPipeItem*>(base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_DATA:
            main_channel_marshall_agent_data(this, m,
                static_cast<RedAgentDataPipeItem*>(base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_DATA:
            main_channel_marshall_migrate_data_item(this, m, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_INIT:
            priv->init_sent = TRUE;
            main_channel_marshall_init(this, m,
                static_cast<RedInitPipeItem*>(base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_NOTIFY:
            main_channel_marshall_notify(this, m,
                static_cast<RedNotifyPipeItem*>(base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN:
            main_channel_marshall_migrate_begin(m, this, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN_SEAMLESS:
            main_channel_marshall_migrate_begin_seamless(m, this, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MULTI_MEDIA_TIME:
            main_channel_marshall_multi_media_time(this, m,
                static_cast<RedMultiMediaTimePipeItem*>(base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_SWITCH_HOST:
            main_channel_marshall_migrate_switch(m, this, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_NAME:
            init_send_data(SPICE_MSG_MAIN_NAME);
            spice_marshall_msg_main_name(m, &static_cast<RedNamePipeItem*>(base)->msg);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_UUID:
            init_send_data(SPICE_MSG_MAIN_UUID);
            spice_marshall_msg_main_uuid(m, &static_cast<RedUuidPipeItem*>(base)->msg);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_CONNECTED_TOKENS:
            main_channel_marshall_agent_connected(m, this, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_REGISTERED_CHANNEL:
            /* The spice protocol requires that the server receive a ATTACH_CHANNELS
             * message from the client before sending any CHANNEL_LIST message. If
             * we've already sent our initial CHANNELS_LIST message, then it should be
             * safe to send new ones for newly-registered channels. */
            if (!priv->initial_channels_list_sent) {
                return;
            }
            main_channel_marshall_registered_channel(this, m,
                static_cast<RedRegisteredChannelPipeItem*>(base));
            break;
        default:
            break;
    };
    begin_send_message();
}
