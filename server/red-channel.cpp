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


    Author:
        yhalperi@redhat.com
*/
#include <config.h>

#include <common/ring.h>

#include "red-channel.h"
#include "red-channel-client.h"
#include "reds.h"
#include "red-stream.h"
#include "main-dispatcher.h"
#include "utils.h"
#include "utils.hpp"

/*
 * Lifetime of RedChannel, RedChannelClient and RedClient:
 * RedChannel is created and destroyed by the calls to
 * red_channel_create.* and red_channel_destroy. The RedChannel resources
 * are deallocated only after red_channel_destroy is called and no RedChannelClient
 * refers to the channel.
 * RedChannelClient is created and destroyed by the calls to xxx_channel_client_new
 * and RedChannelClient::disconnect. RedChannelClient resources are deallocated only when
 * its refs == 0. The reference count of RedChannelClient can be increased by routines
 * that include calls that might destroy the red_channel_client. For example,
 * red_peer_handle_incoming calls the handle_message proc of the channel, which
 * might lead to destroying the client. However, after the call to handle_message,
 * there is a call to the channel's release_msg_buf proc.
 *
 * Once RedChannelClient::disconnect is called, the RedChannelClient is disconnected and
 * removed from the RedChannel clients list, but if rcc->refs != 0, it will still hold
 * a reference to the Channel. The reason for this is that on the one hand RedChannel holds
 * callbacks that may be still in use by RedChannel, and on the other hand,
 * when an operation is performed on the list of clients that belongs to the channel,
 * we don't want to execute it on the "to be destroyed" channel client.
 *
 * RedClient is created and destroyed by the calls to red_client_new and red_client_destroy.
 * When it is destroyed, it also disconnects and destroys all the RedChannelClients that
 * are associated with it. However, since part of these channel clients may still have
 * other references, they will not be completely released, until they are dereferenced.
 *
 * Note: RedChannelClient::disconnect is not thread safe.
 * If a call to RedChannelClient::disconnect is made from another location, it must be called
 * from the channel's thread.
*/
struct RedChannelPrivate
{
    uint32_t type;
    uint32_t id;

    /* "core" interface to register events.
     * Can be thread specific.
     */
    SpiceCoreInterfaceInternal *core;
    gboolean handle_acks;

    // RedChannel will hold only connected channel clients
    // (logic - when pushing pipe item to all channel clients, there
    // is no need to go over disconnect clients)
    // . While client will hold the channel clients till it is destroyed
    // and then it will destroy them as well.
    // However RCC still holds a reference to the Channel.
    // Maybe replace these logic with ref count?
    // TODO: rename to 'connected_clients'?
    GList *clients;

    RedChannelCapabilities local_caps;
    uint32_t migration_flags;

    // TODO: when different channel_clients are in different threads
    // from Channel -> need to protect!
    pthread_t thread_id;
    /* Setting dispatcher allows the channel to execute code in the right
     * thread.
     * thread_id will be used to check the channel thread and automatically
     * use the dispatcher if the thread is different.
     */
    Dispatcher *dispatcher;
    RedsState *reds;
    RedStatNode stat;
};

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(RedChannel, red_channel, G_TYPE_OBJECT)

enum {
    PROP0,
    PROP_SPICE_SERVER,
    PROP_CORE_INTERFACE,
    PROP_TYPE,
    PROP_ID,
    PROP_HANDLE_ACKS,
    PROP_MIGRATION_FLAGS,
    PROP_DISPATCHER,
};

static void
red_channel_get_property(GObject *object,
                         guint property_id,
                         GValue *value,
                         GParamSpec *pspec)
{
    RedChannel *self = RED_CHANNEL(object);

    switch (property_id)
    {
        case PROP_SPICE_SERVER:
            g_value_set_pointer(value, self->priv->reds);
            break;
        case PROP_CORE_INTERFACE:
            g_value_set_pointer(value, self->priv->core);
            break;
        case PROP_TYPE:
            g_value_set_int(value, self->priv->type);
            break;
        case PROP_ID:
            g_value_set_uint(value, self->priv->id);
            break;
        case PROP_HANDLE_ACKS:
            g_value_set_boolean(value, self->priv->handle_acks);
            break;
        case PROP_MIGRATION_FLAGS:
            g_value_set_uint(value, self->priv->migration_flags);
            break;
        case PROP_DISPATCHER:
            g_value_set_object(value, self->priv->dispatcher);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
red_channel_set_property(GObject *object,
                         guint property_id,
                         const GValue *value,
                         GParamSpec *pspec)
{
    RedChannel *self = RED_CHANNEL(object);

    switch (property_id)
    {
        case PROP_SPICE_SERVER:
            self->priv->reds = (RedsState*) g_value_get_pointer(value);
            break;
        case PROP_CORE_INTERFACE:
            self->priv->core = (SpiceCoreInterfaceInternal*) g_value_get_pointer(value);
            break;
        case PROP_TYPE:
            self->priv->type = g_value_get_int(value);
            break;
        case PROP_ID:
            self->priv->id = g_value_get_uint(value);
            break;
        case PROP_HANDLE_ACKS:
            self->priv->handle_acks = g_value_get_boolean(value);
            break;
        case PROP_MIGRATION_FLAGS:
            self->priv->migration_flags = g_value_get_uint(value);
            break;
        case PROP_DISPATCHER:
            g_clear_object(&self->priv->dispatcher);
            self->priv->dispatcher = (Dispatcher*) g_value_dup_object(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
red_channel_finalize(GObject *object)
{
    RedChannel *self = RED_CHANNEL(object);

    g_clear_object(&self->priv->dispatcher);
    red_channel_capabilities_reset(&self->priv->local_caps);

    G_OBJECT_CLASS(red_channel_parent_class)->finalize(object);
}

static void
red_channel_constructed(GObject *object)
{
    RedChannel *self = RED_CHANNEL(object);

    red_channel_debug(self, "thread_id %p", (void*) self->priv->thread_id);

    RedChannelClass *klass = RED_CHANNEL_GET_CLASS(self);

    G_OBJECT_CLASS(red_channel_parent_class)->constructed(object);

    spice_assert(klass->parser != NULL);
}

static void red_channel_client_default_connect(RedChannel *channel, RedClient *client,
                                               RedStream *stream,
                                               int migration,
                                               RedChannelCapabilities *caps)
{
    spice_error("not implemented");
}

static void
red_channel_class_init(RedChannelClass *klass)
{
    GParamSpec *spec;
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = red_channel_get_property;
    object_class->set_property = red_channel_set_property;
    object_class->finalize = red_channel_finalize;
    object_class->constructed = red_channel_constructed;

    spec = g_param_spec_pointer("spice-server",
                                "spice-server",
                                "The spice server associated with this channel",
                                G_PARAM_READWRITE |
                                G_PARAM_CONSTRUCT_ONLY |
                                G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_SPICE_SERVER, spec);

    spec = g_param_spec_pointer("core-interface",
                                "core-interface",
                                "The SpiceCoreInterface server associated with this channel",
                                G_PARAM_READWRITE |
                                G_PARAM_CONSTRUCT_ONLY |
                                G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_CORE_INTERFACE, spec);

    /* FIXME: generate enums for this in spice-common? */
    spec = g_param_spec_int("channel-type",
                            "channel type",
                            "Type of this channel",
                            0,
                            SPICE_END_CHANNEL,
                            0,
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY |
                            G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_TYPE, spec);

    spec = g_param_spec_uint("id",
                             "id",
                             "ID of this channel",
                             0,
                             G_MAXUINT,
                             0,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_ID, spec);

    spec = g_param_spec_boolean("handle-acks",
                                "Handle ACKs",
                                "Whether this channel handles ACKs",
                                FALSE,
                                G_PARAM_READWRITE |
                                G_PARAM_CONSTRUCT_ONLY |
                                G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_HANDLE_ACKS, spec);

    spec = g_param_spec_uint("migration-flags",
                             "migration flags",
                             "Migration flags for this channel",
                             0,
                             G_MAXUINT,
                             0,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_MIGRATION_FLAGS, spec);

    spec = g_param_spec_object("dispatcher", "dispatcher",
                               "Dispatcher bound to channel thread",
                               TYPE_DISPATCHER,
                               G_PARAM_STATIC_STRINGS
                               | G_PARAM_READWRITE
                               | G_PARAM_CONSTRUCT_ONLY);
    g_object_class_install_property(object_class, PROP_DISPATCHER, spec);

    klass->connect = red_channel_client_default_connect;
}

static void
red_channel_init(RedChannel *self)
{
    self->priv = (RedChannelPrivate*) red_channel_get_instance_private(self);

    self->set_common_cap(SPICE_COMMON_CAP_MINI_HEADER);
    self->set_common_cap(SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
    self->priv->thread_id = pthread_self();
}

// utility to avoid possible invalid function cast
static void
red_channel_foreach_client(RedChannel *channel, void (RedChannelClient::*func)())
{
    RedChannelClient *client;
    GLIST_FOREACH(channel->priv->clients, RedChannelClient, client) {
        (client->*func)();
    }
}

void RedChannel::receive()
{
    red_channel_foreach_client(this, &RedChannelClient::receive);
}

void RedChannel::add_client(RedChannelClient *rcc)
{
    spice_assert(rcc);
    priv->clients = g_list_prepend(priv->clients, rcc);
}

bool RedChannel::test_remote_cap(uint32_t cap)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(this, rcc) {
        if (!rcc->test_remote_cap(cap)) {
            return FALSE;
        }
    }
    return TRUE;
}

bool RedChannel::is_waiting_for_migrate_data()
{
    RedChannelClient *rcc;
    guint n_clients = g_list_length(priv->clients);

    if (!is_connected()) {
        return FALSE;
    }

    if (n_clients > 1) {
        return FALSE;
    }
    spice_assert(n_clients == 1);
    rcc = (RedChannelClient*) g_list_nth_data(priv->clients, 0);
    return rcc->is_waiting_for_migrate_data();
}

void RedChannel::init_stat_node(const RedStatNode *parent, const char *name)
{
    // TODO check not already initialized
    stat_init_node(&priv->stat, priv->reds, parent, name, TRUE);
}

const RedStatNode *RedChannel::get_stat_node()
{
    return &priv->stat;
}

static void add_capability(uint32_t **caps, int *num_caps, uint32_t cap)
{
    int nbefore, n;

    nbefore = *num_caps;
    n = cap / 32;
    *num_caps = MAX(*num_caps, n + 1);
    *caps = spice_renew(uint32_t, *caps, *num_caps);
    memset(*caps + nbefore, 0, (*num_caps - nbefore) * sizeof(uint32_t));
    (*caps)[n] |= (1 << (cap % 32));
}

void RedChannel::set_common_cap(uint32_t cap)
{
    add_capability(&priv->local_caps.common_caps,
                   &priv->local_caps.num_common_caps, cap);
}

void RedChannel::set_cap(uint32_t cap)
{
    add_capability(&priv->local_caps.caps, &priv->local_caps.num_caps, cap);
}

void RedChannel::destroy()
{
    // prevent future connection
    reds_unregister_channel(priv->reds, this);

    red_channel_foreach_client(this, &RedChannelClient::disconnect);
    g_object_unref(this);
}

void RedChannel::send()
{
    red_channel_foreach_client(this, &RedChannelClient::send);
}

void RedChannel::push()
{
    red_channel_foreach_client(this, &RedChannelClient::push);
}

void RedChannel::pipes_add(RedPipeItem *item)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(this, rcc) {
        red_pipe_item_ref(item);
        rcc->pipe_add(item);
    }

    red_pipe_item_unref(item);
}

void RedChannel::pipes_add_type(int pipe_item_type)
{
    RedPipeItem *item = g_new(RedPipeItem, 1);

    red_pipe_item_init(item, pipe_item_type);

    pipes_add(item);
}

void RedChannel::pipes_add_empty_msg(int msg_type)
{
    pipes_add(RedChannelClient::new_empty_msg(msg_type));
}

int RedChannel::is_connected()
{
    return priv->clients != NULL;
}

const char *RedChannel::get_name() const
{
    return red_channel_type_to_str(priv->type);
}

void RedChannel::remove_client(RedChannelClient *rcc)
{
    GList *link;
    g_return_if_fail(this == rcc->get_channel());

    if (!pthread_equal(pthread_self(), priv->thread_id)) {
        red_channel_warning(this,
                            "channel->thread_id (%p) != "
                            "pthread_self (%p)."
                            "If one of the threads is != io-thread && != vcpu-thread, "
                            "this might be a BUG",
                            (void*) priv->thread_id, (void*) pthread_self());
    }
    link = g_list_find(priv->clients, rcc);
    spice_return_if_fail(link != NULL);

    priv->clients = g_list_delete_link(priv->clients, link);
    // TODO: should we set rcc->channel to NULL???
}

void RedChannel::disconnect()
{
    red_channel_foreach_client(this, &RedChannelClient::disconnect);
}

typedef struct RedMessageConnect {
    RedChannel *channel;
    RedClient *client;
    RedStream *stream;
    int migration;
    RedChannelCapabilities caps;
} RedMessageConnect;

static void handle_dispatcher_connect(void *opaque, void *payload)
{
    RedMessageConnect *msg = (RedMessageConnect*) payload;
    RedChannel *channel = msg->channel;
    RedChannelClass *klass = RED_CHANNEL_GET_CLASS(channel);

    klass->connect(channel, msg->client, msg->stream, msg->migration, &msg->caps);
    g_object_unref(msg->client);
    red_channel_capabilities_reset(&msg->caps);
}

void RedChannel::connect(RedClient *client, RedStream *stream, int migration,
                         RedChannelCapabilities *caps)
{
    if (priv->dispatcher == NULL ||
        pthread_equal(pthread_self(), priv->thread_id)) {
        RedChannelClass *klass = RED_CHANNEL_GET_CLASS(this);
        klass->connect(this, client, stream, migration, caps);
        return;
    }

    Dispatcher *dispatcher = priv->dispatcher;

    // get a reference potentially the main channel can be destroyed in
    // the main thread causing RedClient to be destroyed before using it
    RedMessageConnect payload = {
        .channel = this,
        .client = (RedClient*) g_object_ref(client),
        .stream = stream,
        .migration = migration
    };
    red_channel_capabilities_init(&payload.caps, caps);

    dispatcher_send_message_custom(dispatcher, handle_dispatcher_connect,
                                   &payload, sizeof(payload), false);
}

GList *RedChannel::get_clients()
{
    return priv->clients;
}
guint RedChannel::get_n_clients()
{
    return g_list_length(priv->clients);
}

bool RedChannel::all_blocked()
{
    RedChannelClient *rcc;

    if (!priv->clients) {
        return FALSE;
    }
    FOREACH_CLIENT(this, rcc) {
        if (!rcc->is_blocked()) {
            return FALSE;
        }
    }
    return TRUE;
}

/* return TRUE if any of the connected clients to this channel are blocked */
static bool red_channel_any_blocked(RedChannel *channel)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(channel, rcc) {
        if (rcc->is_blocked()) {
            return TRUE;
        }
    }
    return FALSE;
}

static bool red_channel_no_item_being_sent(RedChannel *channel)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(channel, rcc) {
        if (!rcc->no_item_being_sent()) {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * Functions to push the same item to multiple pipes.
 */

/*
 * TODO: after convinced of correctness, add paths for single client
 * that avoid the whole loop. perhaps even have a function pointer table
 * later.
 * TODO - inline? macro? right now this is the simplest from code amount
 */

/**
 * red_channel_pipes_new_add:
 * @channel: a channel
 * @creator: a callback to create pipe item (not null)
 * @data: the data to pass to the creator
 *
 * Returns: the number of added items
 **/
int RedChannel::pipes_new_add(new_pipe_item_t creator, void *data)
{
    RedChannelClient *rcc;
    RedPipeItem *item;
    int num = 0, n = 0;

    spice_assert(creator != NULL);

    FOREACH_CLIENT(this, rcc) {
        item = (*creator)(rcc, data, num++);
        if (item) {
            rcc->pipe_add(item);
            n++;
        }
    }

    return n;
}

uint32_t RedChannel::max_pipe_size()
{
    RedChannelClient *rcc;
    uint32_t pipe_size = 0;

    FOREACH_CLIENT(this, rcc) {
        uint32_t new_size;
        new_size = rcc->get_pipe_size();
        pipe_size = MAX(pipe_size, new_size);
    }
    return pipe_size;
}

uint32_t RedChannel::sum_pipes_size()
{
    RedChannelClient *rcc;
    uint32_t sum = 0;

    FOREACH_CLIENT(this, rcc) {
        sum += rcc->get_pipe_size();
    }
    return sum;
}

static void red_channel_disconnect_if_pending_send(RedChannel *channel)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(channel, rcc) {
        if (rcc->is_blocked() || !rcc->pipe_is_empty()) {
            rcc->disconnect();
        } else {
            spice_assert(rcc->no_item_being_sent());
        }
    }
}

bool RedChannel::wait_all_sent(int64_t timeout)
{
    uint64_t end_time;
    uint32_t max_pipe_size;
    int blocked = FALSE;

    if (timeout != -1) {
        end_time = spice_get_monotonic_time_ns() + timeout;
    } else {
        end_time = UINT64_MAX;
    }

    push();
    while (((max_pipe_size = this->max_pipe_size()) ||
            (blocked = red_channel_any_blocked(this))) &&
           (timeout == -1 || spice_get_monotonic_time_ns() < end_time)) {
        spice_debug("pipe-size %u blocked %d", max_pipe_size, blocked);
        usleep(CHANNEL_BLOCKED_SLEEP_DURATION);
        receive();
        send();
        push();
    }

    if (max_pipe_size || blocked) {
        spice_warning("timeout: pending out messages exist (pipe-size %u, blocked %d)",
                      max_pipe_size, blocked);
        red_channel_disconnect_if_pending_send(this);
        return FALSE;
    } else {
        spice_assert(red_channel_no_item_being_sent(this));
        return TRUE;
    }
}

RedsState* RedChannel::get_server()
{
    return priv->reds;
}

SpiceCoreInterfaceInternal* RedChannel::get_core_interface()
{
    return priv->core;
}

void RedChannel::reset_thread_id()
{
    priv->thread_id = pthread_self();
}

const RedChannelCapabilities* RedChannel::get_local_capabilities()
{
    return &priv->local_caps;
}

typedef struct RedMessageMigrate {
    RedChannelClient *rcc;
} RedMessageMigrate;

static void handle_dispatcher_migrate(void *opaque, void *payload)
{
    RedMessageMigrate *msg = (RedMessageMigrate*) payload;

    msg->rcc->migrate();
    msg->rcc->unref();
}

void RedChannel::migrate_client(RedChannelClient *rcc)
{
    if (priv->dispatcher == NULL ||
        pthread_equal(pthread_self(), priv->thread_id)) {
        rcc->migrate();
        return;
    }

    RedMessageMigrate payload = { .rcc = red::add_ref(rcc) };
    dispatcher_send_message_custom(priv->dispatcher, handle_dispatcher_migrate,
                                   &payload, sizeof(payload), false);
}

typedef struct RedMessageDisconnect {
    RedChannelClient *rcc;
} RedMessageDisconnect;

static void handle_dispatcher_disconnect(void *opaque, void *payload)
{
    RedMessageDisconnect *msg = (RedMessageDisconnect*) payload;

    msg->rcc->disconnect();
}

void RedChannel::disconnect_client(RedChannelClient *rcc)
{
    if (priv->dispatcher == NULL ||
        pthread_equal(pthread_self(), priv->thread_id)) {
        rcc->disconnect();
        return;
    }

    // TODO: we turned it to be sync, due to client_destroy . Should we support async? - for this we will need ref count
    // for channels
    RedMessageDisconnect payload = { .rcc = rcc };
    dispatcher_send_message_custom(priv->dispatcher, handle_dispatcher_disconnect,
                                   &payload, sizeof(payload), true);
}
