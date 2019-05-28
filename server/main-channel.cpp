/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

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

#include <common/ring.h>

#include "red-common.h"
#include "reds.h"
#include "red-channel-client.h"
#include "red-client.h"
#include "main-channel.h"
#include "main-channel-client.h"

struct MainChannel final: public RedChannel
{
    // TODO: add refs and release (afrer all clients completed migration in one way or the other?)
    RedsMigSpice mig_target;
    int num_clients_mig_wait;
};

struct MainChannelClass
{
    RedChannelClass parent_class;
};

G_DEFINE_TYPE(MainChannel, main_channel, RED_TYPE_CHANNEL)

int main_channel_is_connected(MainChannel *main_chan)
{
    return main_chan && main_chan->is_connected();
}

XXX_CAST(RedChannelClient, MainChannelClient, MAIN_CHANNEL_CLIENT);

RedClient *main_channel_get_client_by_link_id(MainChannel *main_chan, uint32_t connection_id)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(main_chan, rcc) {
        MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
        if (main_channel_client_get_connection_id(mcc) == connection_id) {
            return rcc->get_client();
        }
    }
    return NULL;
}

static void main_channel_push_channels(MainChannelClient *mcc)
{
    if (red_client_during_migrate_at_target(mcc->get_client())) {
        red_channel_warning(mcc->get_channel(),
                            "warning: ignoring unexpected SPICE_MSGC_MAIN_ATTACH_CHANNELS"
                            "during migration");
        return;
    }
    mcc->pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_CHANNELS_LIST);
}

void main_channel_push_mouse_mode(MainChannel *main_chan, SpiceMouseMode current_mode,
                                  int is_client_mouse_allowed)
{
    main_chan->pipes_add(main_mouse_mode_item_new(current_mode, is_client_mouse_allowed));
}

void main_channel_push_agent_connected(MainChannel *main_chan)
{
    RedChannelClient *rcc;
    FOREACH_CLIENT(main_chan, rcc) {
        if (rcc->test_remote_cap(SPICE_MAIN_CAP_AGENT_CONNECTED_TOKENS)) {
            rcc->pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_AGENT_CONNECTED_TOKENS);
        } else {
            rcc->pipe_add_empty_msg(SPICE_MSG_MAIN_AGENT_CONNECTED);
        }
    }
}

void main_channel_push_agent_disconnected(MainChannel *main_chan)
{
    main_chan->pipes_add_type(RED_PIPE_ITEM_TYPE_MAIN_AGENT_DISCONNECTED);
}

static void main_channel_push_migrate_data_item(MainChannel *main_chan)
{
    main_chan->pipes_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_DATA);
}

bool MainChannelClient::handle_migrate_data(uint32_t size, void *message)
{
    RedChannel *channel = get_channel();
    MainChannelClient *mcc = this;
    SpiceMigrateDataHeader *header = (SpiceMigrateDataHeader *)message;

    /* not supported with multi-clients */
    spice_assert(channel->get_n_clients() == 1);

    if (size < sizeof(SpiceMigrateDataHeader) + sizeof(SpiceMigrateDataMain)) {
        red_channel_warning(channel, "bad message size %u", size);
        return FALSE;
    }
    if (!migration_protocol_validate_header(header,
                                            SPICE_MIGRATE_DATA_MAIN_MAGIC,
                                            SPICE_MIGRATE_DATA_MAIN_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }
    return reds_handle_migrate_data(channel->get_server(), mcc,
                                    (SpiceMigrateDataMain *)(header + 1),
                                    size);
}

void main_channel_push_multi_media_time(MainChannel *main_chan, uint32_t time)
{
    main_chan->pipes_add(main_multi_media_time_item_new(time));
}

static void main_channel_fill_mig_target(MainChannel *main_channel, RedsMigSpice *mig_target)
{
    spice_assert(mig_target);
    g_free(main_channel->mig_target.host);
    main_channel->mig_target.host = g_strdup(mig_target->host);
    g_free(main_channel->mig_target.cert_subject);
    if (mig_target->cert_subject) {
        main_channel->mig_target.cert_subject = g_strdup(mig_target->cert_subject);
    } else {
        main_channel->mig_target.cert_subject = NULL;
    }
    main_channel->mig_target.port = mig_target->port;
    main_channel->mig_target.sport = mig_target->sport;
}

void
main_channel_registered_new_channel(MainChannel *main_chan, RedChannel *channel)
{
    main_chan->pipes_add(registered_channel_item_new(channel));
}

void main_channel_migrate_switch(MainChannel *main_chan, RedsMigSpice *mig_target)
{
    main_channel_fill_mig_target(main_chan, mig_target);
    main_chan->pipes_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_SWITCH_HOST);
}

bool MainChannelClient::handle_message(uint16_t type, uint32_t size, void *message)
{
    RedChannel *channel = get_channel();
    RedsState *reds = channel->get_server();

    switch (type) {
    case SPICE_MSGC_MAIN_AGENT_START: {
        SpiceMsgcMainAgentStart *tokens;

        tokens = (SpiceMsgcMainAgentStart *)message;
        reds_on_main_agent_start(reds, this, tokens->num_tokens);
        break;
    }
    case SPICE_MSGC_MAIN_AGENT_DATA:
        reds_on_main_agent_data(reds, this, message, size);
        break;
    case SPICE_MSGC_MAIN_AGENT_TOKEN: {
        SpiceMsgcMainAgentTokens *tokens;

        tokens = (SpiceMsgcMainAgentTokens *)message;
        reds_on_main_agent_tokens(reds, this, tokens->num_tokens);
        break;
    }
    case SPICE_MSGC_MAIN_ATTACH_CHANNELS:
        main_channel_push_channels(this);
        break;
    case SPICE_MSGC_MAIN_MIGRATE_CONNECTED:
        main_channel_client_handle_migrate_connected(this,
                                                     TRUE /* success */,
                                                     FALSE /* seamless */);
        break;
    case SPICE_MSGC_MAIN_MIGRATE_CONNECTED_SEAMLESS:
        main_channel_client_handle_migrate_connected(this,
                                                     TRUE /* success */,
                                                     TRUE /* seamless */);
        break;
    case SPICE_MSGC_MAIN_MIGRATE_CONNECT_ERROR:
        main_channel_client_handle_migrate_connected(this, FALSE, FALSE);
        break;
    case SPICE_MSGC_MAIN_MIGRATE_DST_DO_SEAMLESS:
        main_channel_client_handle_migrate_dst_do_seamless(this,
            ((SpiceMsgcMainMigrateDstDoSeamless *)message)->src_version);
        break;
    case SPICE_MSGC_MAIN_MIGRATE_END:
        main_channel_client_handle_migrate_end(this);
        break;
    case SPICE_MSGC_MAIN_MOUSE_MODE_REQUEST:
        reds_on_main_mouse_mode_request(reds, message, size);
        break;
    case SPICE_MSGC_PONG:
        main_channel_client_handle_pong(this, (SpiceMsgPing *)message, size);
        break;
    default:
        return RedChannelClient::handle_message(type, size, message);
    }
    return TRUE;
}

void MainChannelClient::handle_migrate_flush_mark()
{
    RedChannel *channel = get_channel();
    spice_debug("trace");
    main_channel_push_migrate_data_item(MAIN_CHANNEL(channel));
}

MainChannelClient *main_channel_link(MainChannel *channel, RedClient *client,
                                     RedStream *stream, uint32_t connection_id, int migration,
                                     RedChannelCapabilities *caps)
{
    MainChannelClient *mcc;

    spice_assert(channel);

    // TODO - migration - I removed it from channel creation, now put it
    // into usage somewhere (not an issue until we return migration to it's
    // former glory)
    mcc = main_channel_client_create(channel, client, stream, connection_id, caps);
    return mcc;
}

MainChannel* main_channel_new(RedsState *reds)
{
    // TODO: set the migration flag of the channel
    return (MainChannel*) g_object_new(TYPE_MAIN_CHANNEL,
                        "spice-server", reds,
                        "core-interface", reds_get_core_interface(reds),
                        "channel-type", (gint)SPICE_CHANNEL_MAIN,
                        "id", 0,
                        "migration-flags",
                        (SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER),
                        NULL);
}

static void
main_channel_constructed(GObject *object)
{
    MainChannel *self = MAIN_CHANNEL(object);

    G_OBJECT_CLASS(main_channel_parent_class)->constructed(object);

    self->set_cap(SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE);
    self->set_cap(SPICE_MAIN_CAP_SEAMLESS_MIGRATE);
}

static void
main_channel_init(MainChannel *self)
{
}

static void
main_channel_class_init(MainChannelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    object_class->constructed = main_channel_constructed;

    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_MAIN, NULL);
}

static int main_channel_connect_semi_seamless(MainChannel *main_channel)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(main_channel, rcc) {
        MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
        if (main_channel_client_connect_semi_seamless(mcc))
            main_channel->num_clients_mig_wait++;
    }
    return main_channel->num_clients_mig_wait;
}

static int main_channel_connect_seamless(MainChannel *main_channel)
{
    RedChannelClient *rcc;

    spice_assert(main_channel->get_n_clients() == 1);

    FOREACH_CLIENT(main_channel, rcc) {
        MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
        main_channel_client_connect_seamless(mcc);
        main_channel->num_clients_mig_wait++;
    }
    return main_channel->num_clients_mig_wait;
}

int main_channel_migrate_connect(MainChannel *main_channel, RedsMigSpice *mig_target,
                                 int try_seamless)
{
    main_channel_fill_mig_target(main_channel, mig_target);
    main_channel->num_clients_mig_wait = 0;

    if (!main_channel_is_connected(main_channel)) {
        return 0;
    }

    if (!try_seamless) {
        return main_channel_connect_semi_seamless(main_channel);
    } else {
        RedChannelClient *rcc;
        GList *clients = main_channel->get_clients();

        /* just test the first one */
        rcc = (RedChannelClient*) g_list_nth_data(clients, 0);

        if (!rcc->test_remote_cap(SPICE_MAIN_CAP_SEAMLESS_MIGRATE)) {
            return main_channel_connect_semi_seamless(main_channel);
        } else {
            return main_channel_connect_seamless(main_channel);
        }
    }

}

void main_channel_migrate_cancel_wait(MainChannel *main_chan)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(main_chan, rcc) {
        MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
        main_channel_client_migrate_cancel_wait(mcc);
    }
    main_chan->num_clients_mig_wait = 0;
}

int main_channel_migrate_src_complete(MainChannel *main_chan, int success)
{
    int semi_seamless_count = 0;
    RedChannelClient *rcc;

    if (!main_chan->get_clients()) {
        red_channel_warning(main_chan, "no peer connected");
        return 0;
    }

    FOREACH_CLIENT(main_chan, rcc) {
        MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
        if (main_channel_client_migrate_src_complete(mcc, success))
            semi_seamless_count++;
   }
   return semi_seamless_count;
}

void main_channel_on_migrate_connected(MainChannel *main_channel,
                                       gboolean success, gboolean seamless)
{
        spice_assert(main_channel->num_clients_mig_wait);
        spice_assert(!seamless || main_channel->num_clients_mig_wait == 1);
        if (!--main_channel->num_clients_mig_wait) {
            reds_on_main_migrate_connected(main_channel->get_server(),
                                           seamless && success);
        }
}

const RedsMigSpice* main_channel_get_migration_target(MainChannel *main_chan)
{
    return &main_chan->mig_target;
}
