/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2017 Red Hat, Inc.

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
/*
 * This test allocate a channel and do some test sending some messages
 */
#include <config.h>
#include <unistd.h>
#include <spice.h>

#include "test-glib-compat.h"
#include "basic-event-loop.h"
#include "reds.h"
#include "red-client.h"
#include "cursor-channel.h"
#include "net-utils.h"
#include "win-alarm.h"

/*
 * Declare a RedTestChannel to be used for the test
 */
SPICE_DECLARE_TYPE(RedTestChannel, red_test_channel, TEST_CHANNEL);
#define RED_TYPE_TEST_CHANNEL red_test_channel_get_type()

struct RedTestChannel final: public RedChannel
{
};

struct RedTestChannelClass
{
    RedChannelClass parent_class;
};

G_DEFINE_TYPE(RedTestChannel, red_test_channel, RED_TYPE_CHANNEL)

class RedTestChannelClient final: public RedChannelClient
{
    using RedChannelClient::RedChannelClient;
    virtual uint8_t * alloc_recv_buf(uint16_t type, uint32_t size) override;
    virtual void release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg) override;
};

static void
red_test_channel_init(RedTestChannel *self)
{
}

static void
test_connect_client(RedChannel *channel, RedClient *client, RedStream *stream,
                    int migration, RedChannelCapabilities *caps)
{
    RedChannelClient *rcc;
    rcc = new RedTestChannelClient(channel, client, stream, caps);
    g_assert_nonnull(rcc);
    g_assert_true(rcc->init());

    // requires an ACK after 10 messages
    rcc->ack_set_client_window(10);

    // initialize ACK feature
    rcc->ack_zero_messages_window();
    rcc->push_set_ack();

    // send enough messages till we should require an ACK
    // the ACK is waited after 2 * 10, append some other messages
    for (int i = 0; i < 25; ++i) {
        rcc->pipe_add_empty_msg(SPICE_MSG_MIGRATE_DATA);
    }
}

static void
red_test_channel_class_init(RedTestChannelClass *klass)
{
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);
    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_PORT, NULL);
    channel_class->connect = test_connect_client;
}

uint8_t *
RedTestChannelClient::alloc_recv_buf(uint16_t type, uint32_t size)
{
    return (uint8_t*) g_malloc(size);
}

void
RedTestChannelClient::release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg)
{
    g_free(msg);
}


/*
 * Main test part
 */
typedef SpiceWatch *watch_add_t(const SpiceCoreInterfaceInternal *iface,
                                int fd, int event_mask, SpiceWatchFunc func, void *opaque);
static watch_add_t *old_watch_add = NULL;
static SpiceWatchFunc old_watch_func = NULL;

static int watch_called_countdown = 5;

// this function is injected in the RedChannelClient watch function
static void watch_func_inject(int fd, int event, void *opaque)
{
    // check we are not doing too many loops
    if (--watch_called_countdown <= 0) {
        spice_error("Watch called too many times");
    }
    old_watch_func(fd, event, opaque);
}

static SpiceWatch *
watch_add_inject(const SpiceCoreInterfaceInternal *iface,
                 int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    g_assert_null(old_watch_func);
    old_watch_func = func;
    SpiceWatch* ret = old_watch_add(iface, fd, event_mask, watch_func_inject, opaque);
    return ret;
}

static int client_socket = -1;

static void send_ack_sync(int socket, uint32_t generation)
{
    struct {
        uint16_t dummy;
        uint16_t type;
        uint32_t len;
        uint32_t generation;
    } msg;
    SPICE_VERIFY(sizeof(msg) == 12);
    msg.type = GUINT16_TO_LE(SPICE_MSGC_ACK_SYNC);
    msg.len = GUINT32_TO_LE(sizeof(generation));
    msg.generation = GUINT32_TO_LE(generation);

    g_assert_cmpint(socket_write(socket, &msg.type, 10), ==, 10);
}

static SpiceTimer *waked_up_timer;

// timer waiting we get data again
static void timer_wakeup(void *opaque)
{
    SpiceCoreInterface *core = (SpiceCoreInterface*) opaque;

    // check we are receiving data again
    size_t got_data = 0;
    ssize_t len;
    alarm(1);
    char buffer[256];
    while ((len=socket_read(client_socket, buffer, sizeof(buffer))) > 0)
        got_data += len;
    alarm(0);

    g_assert_cmpint(got_data, >, 0);

    core->timer_remove(waked_up_timer);

    basic_event_loop_quit();
}

// timeout, now we can send the ack
// if we arrive here it means we didn't receive too many watch events
static void timeout_watch_count(void *opaque)
{
    SpiceCoreInterface *core = (SpiceCoreInterface*) opaque;

    // get all pending data
    alarm(1);
    char buffer[256];
    while (socket_read(client_socket, buffer, sizeof(buffer)) > 0)
        continue;
    alarm(0);

    // we don't need to count anymore
    watch_called_countdown = 20;

    // send ack reply, this should unblock data from RedChannelClient
    g_assert_cmpint(socket_write(client_socket, "\2\0\0\0\0\0", 6), ==, 6);

    // expect data soon
    waked_up_timer = core->timer_add(timer_wakeup, core);
    core->timer_start(waked_up_timer, 100);
    // TODO watch
}

static RedStream *create_dummy_stream(SpiceServer *server, int *p_socket)
{
    int sv[2];
    g_assert_cmpint(socketpair(AF_LOCAL, SOCK_STREAM, 0, sv), ==, 0);
    if (p_socket) {
        *p_socket = sv[1];
    }
    red_socket_set_non_blocking(sv[0], true);
    red_socket_set_non_blocking(sv[1], true);

    RedStream * stream = red_stream_new(server, sv[0]);
    g_assert_nonnull(stream);

    return stream;
}

static void channel_loop(void)
{
    SpiceCoreInterface *core;
    SpiceServer *server = spice_server_new();

    g_assert_nonnull(server);

    core = basic_event_loop_init();
    g_assert_nonnull(core);

    g_assert_cmpint(spice_server_init(server, core), ==, 0);

    // create a channel and connect to it
    RedChannel *channel =
        (RedChannel*) g_object_new(RED_TYPE_TEST_CHANNEL,
                     "spice-server", server,
                     "core-interface", reds_get_core_interface(server),
                     "channel-type", SPICE_CHANNEL_PORT, // any other than main is fine
                     "id", 0,
                     "handle-acks", TRUE, // we want to test this
                     NULL);

    // create dummy RedClient and MainChannelClient
    RedChannelCapabilities caps;
    memset(&caps, 0, sizeof(caps));
    uint32_t common_caps = 1 << SPICE_COMMON_CAP_MINI_HEADER;
    caps.num_common_caps = 1;
    caps.common_caps = (uint32_t*) spice_memdup(&common_caps, sizeof(common_caps));

    RedClient *client = red_client_new(server, FALSE);
    g_assert_nonnull(client);

    MainChannel *main_channel = main_channel_new(server);
    g_assert_nonnull(main_channel);

    MainChannelClient *mcc;
    mcc = main_channel_link(main_channel, client, create_dummy_stream(server, NULL),
                            0, FALSE, &caps);
    g_assert_nonnull(mcc);

    // inject a trace into the core interface to count the events
    SpiceCoreInterfaceInternal *server_core = reds_get_core_interface(server);
    old_watch_add = server_core->watch_add;
    server_core->watch_add = watch_add_inject;

    // create our testing RedChannelClient
    channel->connect(client, create_dummy_stream(server, &client_socket),
                     FALSE, &caps);
    red_channel_capabilities_reset(&caps);

    // remove code to inject code during RedChannelClient watch, we set it
    g_assert_nonnull(old_watch_func);
    server_core->watch_add = old_watch_add;

    send_ack_sync(client_socket, 1);

    // set a timeout when to send back the acknowledge,
    // during this time we check not receiving too many events
    SpiceTimer *watch_timer;
    watch_timer = core->timer_add(timeout_watch_count, core);
    core->timer_start(watch_timer, 100);

    // start all test
    basic_event_loop_mainloop();

    // cleanup
    red_client_destroy(client);
    g_object_unref(main_channel);
    g_object_unref(channel);

    core->timer_remove(watch_timer);

    spice_server_destroy(server);

    basic_event_loop_destroy();
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/server/channel", channel_loop);

    return g_test_run();
}
