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

#include "red-channel.h"
#include "red-client.h"
#include "reds.h"

#define FOREACH_CHANNEL_CLIENT(_client, _data) \
    for (const auto &_data: _client->channels)

RedClient::~RedClient()
{
    spice_debug("release client=%p", this);
    pthread_mutex_destroy(&lock);
}

RedClient::RedClient(RedsState *init_reds, bool migrated):
    reds(init_reds),
    during_target_migrate(migrated)
{
    pthread_mutex_init(&lock, nullptr);
    thread_id = pthread_self();
}

RedClient *red_client_new(RedsState *reds, int migrated)
{
    return new RedClient(reds, migrated);
}

void RedClient::set_migration_seamless() // dest
{
    spice_assert(during_target_migrate);
    pthread_mutex_lock(&lock);
    seamless_migrate = TRUE;
    /* update channel clients that got connected before the migration
     * type was set. red_client_add_channel will handle newer channel clients */
    FOREACH_CHANNEL_CLIENT(this, rcc) {
        if (rcc->set_migration_seamless()) {
            num_migrated_channels++;
        }
    }
    pthread_mutex_unlock(&lock);
}

void RedClient::migrate()
{
    if (!pthread_equal(pthread_self(), thread_id)) {
        spice_warning("client->thread_id (%p) != "
                      "pthread_self (%p)."
                      "If one of the threads is != io-thread && != vcpu-thread,"
                      " this might be a BUG",
                      (void*) thread_id, (void*) pthread_self());
    }
    FOREACH_CHANNEL_CLIENT(this, rcc) {
        if (rcc->is_connected()) {
            auto channel = rcc->get_channel();
            channel->migrate_client(rcc.get());
        }
    }
}

void RedClient::destroy()
{
    if (!pthread_equal(pthread_self(), thread_id)) {
        spice_warning("client->thread_id (%p) != "
                      "pthread_self (%p)."
                      "If one of the threads is != io-thread && != vcpu-thread,"
                      " this might be a BUG",
                      (void*) thread_id,
                      (void*) pthread_self());
    }

    pthread_mutex_lock(&lock);
    spice_debug("destroy this %p with #channels=%zd", this, channels.size());
    // This makes sure that we won't try to add new RedChannelClient instances
    // to the RedClient::channels list while iterating it
    disconnecting = TRUE;
    while (!channels.empty()) {
        auto rcc = *channels.begin();

        // Remove the RedChannelClient we are processing from the list
        // Note that we own the object so it is safe to do some operations on it.
        // This manual scan of the list is done to have a thread safe
        // iteration of the list
        channels.pop_front();

        // prevent dead lock disconnecting rcc (which can happen
        // in the same thread or synchronously on another one)
        pthread_mutex_unlock(&lock);

        // some channels may be in other threads, so disconnection
        // is not synchronous.
        auto channel = rcc->get_channel();

        // some channels may be in other threads. However we currently
        // assume disconnect is synchronous (we changed the dispatcher
        // to wait for disconnection)
        // TODO: should we go back to async. For this we need to use
        // ref count for channel clients.
        channel->disconnect_client(rcc.get());

        spice_assert(rcc->pipe_is_empty());
        spice_assert(rcc->no_item_being_sent());

        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    unref();
}


/* client->lock should be locked */
RedChannelClient *RedClient::get_channel(int type, int id)
{
    FOREACH_CHANNEL_CLIENT(this, rcc) {
        RedChannel *channel;

        channel = rcc->get_channel();
        if (channel->type() == type && channel->id() == id) {
            return rcc.get();
        }
    }
    return nullptr;
}

gboolean RedClient::add_channel(RedChannelClient *rcc, char **error)
{
    RedChannel *channel;
    gboolean result = TRUE;

    spice_assert(rcc);
    channel = rcc->get_channel();

    pthread_mutex_lock(&lock);

    uint32_t type = channel->type();
    uint32_t id = channel->id();
    if (disconnecting) {
        *error =
            g_strdup_printf("Client %p got disconnected while connecting channel type %d id %d",
                            this, type, id);
        result = FALSE;
        goto cleanup;
    }

    if (get_channel(type, id)) {
        *error =
            g_strdup_printf("Client %p: duplicate channel type %d id %d",
                            this, type, id);
        result = FALSE;
        goto cleanup;
    }

    // first must be the main one
    if (!mcc) {
        // FIXME use dynamic_cast to check type
        // spice_assert(MAIN_CHANNEL_CLIENT(rcc) != NULL);
        mcc.reset((MainChannelClient *) rcc);
    }
    channels.push_front(red::shared_ptr<RedChannelClient>(rcc));
    if (during_target_migrate && seamless_migrate) {
        if (rcc->set_migration_seamless()) {
            num_migrated_channels++;
        }
    }

cleanup:
    pthread_mutex_unlock(&lock);
    return result;
}

MainChannelClient *RedClient::get_main()
{
    return mcc.get();
}

void RedClient::semi_seamless_migrate_complete()
{
    pthread_mutex_lock(&lock);
    if (!during_target_migrate || seamless_migrate) {
        spice_error("unexpected");
        pthread_mutex_unlock(&lock);
        return;
    }
    during_target_migrate = FALSE;
    FOREACH_CHANNEL_CLIENT(this, rcc) {
        rcc->semi_seamless_migration_complete();
    }
    pthread_mutex_unlock(&lock);
    reds_on_client_semi_seamless_migrate_complete(reds, this);
}

/* should be called only from the main thread */
int RedClient::during_migrate_at_target()
{
    int ret;
    pthread_mutex_lock(&lock);
    ret = during_target_migrate;
    pthread_mutex_unlock(&lock);
    return ret;
}

void RedClient::remove_channel(RedChannelClient *rcc)
{
    RedClient *client = rcc->get_client();
    red::shared_ptr<RedChannelClient> holding_rcc(rcc);
    pthread_mutex_lock(&client->lock);
    client->channels.remove(holding_rcc);
    pthread_mutex_unlock(&client->lock);
}

/* returns TRUE If all channels are finished migrating, FALSE otherwise */
gboolean RedClient::seamless_migration_done_for_channel()
{
    gboolean ret = FALSE;

    pthread_mutex_lock(&lock);
    num_migrated_channels--;
    /* we assume we always have at least one channel who has migration data transfer,
     * otherwise, this flag will never be set back to FALSE*/
    if (!num_migrated_channels) {
        during_target_migrate = FALSE;
        seamless_migrate = FALSE;
        /* migration completion might have been triggered from a different thread
         * than the main thread */
        reds_get_main_dispatcher(reds)->seamless_migrate_dst_complete(this);
        ret = TRUE;
    }
    pthread_mutex_unlock(&lock);

    return ret;
}

gboolean RedClient::is_disconnecting()
{
    gboolean ret;
    pthread_mutex_lock(&lock);
    ret =  disconnecting;
    pthread_mutex_unlock(&lock);
    return ret;
}

void RedClient::set_disconnecting()
{
    pthread_mutex_lock(&lock);
    disconnecting = TRUE;
    pthread_mutex_unlock(&lock);
}

RedsState *RedClient::get_server()
{
    return reds;
}
