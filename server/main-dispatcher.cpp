/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "red-common.h"
#include "dispatcher.h"
#include "main-dispatcher.h"
#include "red-client.h"
#include "reds.h"

/*
 * Main Dispatcher
 * ===============
 *
 * Communication channel between any non main thread and the main thread.
 *
 * The main thread is that from which spice_server_init is called.
 *
 * Messages are single sized, sent from the non-main thread to the main-thread.
 * No acknowledge is sent back. This prevents a possible deadlock with the main
 * thread already waiting on a response for the existing red_dispatcher used
 * by the worker thread.
 *
 * All events have three functions:
 * main_dispatcher_<event_name> - non static, public function
 * main_dispatcher_self_<event_name> - handler for main thread
 * main_dispatcher_handle_<event_name> - handler for callback from main thread
 *   seperate from self because it may send an ack or do other work in the future.
 */

enum {
    MAIN_DISPATCHER_CHANNEL_EVENT = 0,
    MAIN_DISPATCHER_MIGRATE_SEAMLESS_DST_COMPLETE,
    MAIN_DISPATCHER_SET_MM_TIME_LATENCY,
    MAIN_DISPATCHER_CLIENT_DISCONNECT,

    MAIN_DISPATCHER_NUM_MESSAGES
};

struct MainDispatcherChannelEventMessage {
    int event;
    SpiceChannelEventInfo *info;
};

struct MainDispatcherMigrateSeamlessDstCompleteMessage {
    RedClient *client;
};

struct MainDispatcherMmTimeLatencyMessage {
    RedClient *client;
    uint32_t latency;
};

struct MainDispatcherClientDisconnectMessage {
    RedClient *client;
};

/* channel_event - calls core->channel_event, must be done in main thread */
static void main_dispatcher_handle_channel_event(void *opaque,
                                                 void *payload)
{
    auto reds = (RedsState*) opaque;
    auto channel_event = (MainDispatcherChannelEventMessage*) payload;

    reds_handle_channel_event(reds, channel_event->event, channel_event->info);
}

void MainDispatcher::channel_event(int event, SpiceChannelEventInfo *info)
{
    MainDispatcherChannelEventMessage msg = {0,};

    if (pthread_self() == thread_id) {
        reds_handle_channel_event(reds, event, info);
        return;
    }
    msg.event = event;
    msg.info = info;
    send_message(MAIN_DISPATCHER_CHANNEL_EVENT, &msg);
}


static void main_dispatcher_handle_migrate_complete(void *opaque,
                                                    void *payload)
{
    auto reds = (RedsState*) opaque;
    auto mig_complete = (MainDispatcherMigrateSeamlessDstCompleteMessage*) payload;

    reds_on_client_seamless_migrate_complete(reds, mig_complete->client);
    mig_complete->client->unref();
}

static void main_dispatcher_handle_mm_time_latency(void *opaque,
                                                   void *payload)
{
    auto reds = (RedsState*) opaque;
    auto msg = (MainDispatcherMmTimeLatencyMessage*) payload;
    reds_set_client_mm_time_latency(reds, msg->client, msg->latency);
    msg->client->unref();
}

static void main_dispatcher_handle_client_disconnect(void *opaque,
                                                     void *payload)
{
    auto reds = (RedsState*) opaque;
    auto msg = (MainDispatcherClientDisconnectMessage*) payload;

    spice_debug("client=%p", msg->client);
    reds_client_disconnect(reds, msg->client);
    msg->client->unref();
}

void MainDispatcher::seamless_migrate_dst_complete(RedClient *client)
{
    MainDispatcherMigrateSeamlessDstCompleteMessage msg;

    if (pthread_self() == thread_id) {
        reds_on_client_seamless_migrate_complete(reds, client);
        return;
    }

    msg.client = red::add_ref(client);
    send_message(MAIN_DISPATCHER_MIGRATE_SEAMLESS_DST_COMPLETE, &msg);
}

void MainDispatcher::set_mm_time_latency(RedClient *client, uint32_t latency)
{
    MainDispatcherMmTimeLatencyMessage msg;

    if (pthread_self() == thread_id) {
        reds_set_client_mm_time_latency(reds, client, latency);
        return;
    }

    msg.client = red::add_ref(client);
    msg.latency = latency;
    send_message(MAIN_DISPATCHER_SET_MM_TIME_LATENCY, &msg);
}

void MainDispatcher::client_disconnect(RedClient *client)
{
    MainDispatcherClientDisconnectMessage msg;

    if (!client->is_disconnecting()) {
        spice_debug("client %p", client);
        msg.client = red::add_ref(client);
        send_message(MAIN_DISPATCHER_CLIENT_DISCONNECT, &msg);
    } else {
        spice_debug("client %p already during disconnection", client);
    }
}

/*
 * FIXME:
 * Reds routines shouldn't be exposed. Instead reds.cpp should register the callbacks,
 * and the corresponding operations should be made only via main_dispatcher.
 */
MainDispatcher::MainDispatcher(RedsState *init_reds):
    Dispatcher(MAIN_DISPATCHER_NUM_MESSAGES),
    reds(init_reds),
    thread_id(pthread_self())
{
    set_opaque(reds);

    watch = create_watch(reds_get_core_interface(reds));
    register_handler(MAIN_DISPATCHER_CHANNEL_EVENT,
                     main_dispatcher_handle_channel_event,
                     sizeof(MainDispatcherChannelEventMessage), false);
    register_handler(MAIN_DISPATCHER_MIGRATE_SEAMLESS_DST_COMPLETE,
                     main_dispatcher_handle_migrate_complete,
                     sizeof(MainDispatcherMigrateSeamlessDstCompleteMessage), false);
    register_handler(MAIN_DISPATCHER_SET_MM_TIME_LATENCY,
                     main_dispatcher_handle_mm_time_latency,
                     sizeof(MainDispatcherMmTimeLatencyMessage), false);
    register_handler(MAIN_DISPATCHER_CLIENT_DISCONNECT,
                     main_dispatcher_handle_client_disconnect,
                     sizeof(MainDispatcherClientDisconnectMessage), false);
}

MainDispatcher::~MainDispatcher()
{
    red_watch_remove(watch);
}
