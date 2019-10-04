/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2019 Red Hat, Inc.

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
/**
 * Test Smartcard device and channel
 */

#include <config.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#include <spice/protocol.h>
#include <spice/stream-device.h>

#include "test-display-base.h"
#include "test-glib-compat.h"
#include "reds.h"
#include "vmc-emu.h"
#include "red-client.h"
#include "net-utils.h"
#include "win-alarm.h"

static SpiceCoreInterface *core;
static Test *test;
static VmcEmu *vmc;
typedef int TestFixture;

static void test_smartcard_setup(TestFixture *fixture, gconstpointer user_data)
{
    g_assert_null(core);
    g_assert_null(test);
    g_assert_null(vmc);
    core = basic_event_loop_init();
    g_assert_nonnull(core);
    test = test_new(core);
    g_assert_nonnull(test);
    vmc = vmc_emu_new("smartcard", NULL);
    g_assert_nonnull(vmc);
}

static void test_smartcard_teardown(TestFixture *fixture, gconstpointer user_data)
{
    g_assert_nonnull(core);
    g_assert_nonnull(test);
    g_assert_nonnull(vmc);

    vmc_emu_destroy(vmc);
    vmc = NULL;
    test_destroy(test);
    test = NULL;
    basic_event_loop_destroy();
    core = NULL;
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

static void send_data(int socket, uint32_t type)
{
    struct {
        uint16_t dummy;
        uint16_t type;
        uint32_t len;
        VSCMsgHeader vheader;
        char data[6];
    } msg;
    SPICE_VERIFY(sizeof(msg) == 8+12+8);
    msg.type = GUINT16_TO_LE(SPICE_MSGC_SMARTCARD_DATA);
    msg.len = GUINT32_TO_LE(sizeof(VSCMsgHeader)+6);
    msg.vheader.type = GUINT32_TO_LE(type);
    msg.vheader.reader_id = 0;
    msg.vheader.length = GUINT32_TO_LE(6);
    strcpy(msg.data, "hello");

    g_assert_cmpint(socket_write(socket, &msg.type, sizeof(msg)-4), ==, sizeof(msg)-4);
}

static void check_data(void *opaque)
{
    static const char expected_buf[] =
        // forwarded ReaderAdd message, note that payload is stripped
        "\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00\x00"
        // forwarded APDU message
        "\x00\x00\x00\x07\x00\x00\x00\x00\x00\x00\x00\x06\x68\x65\x6c\x6c\x6f\x00";
    const size_t expected_buf_len = sizeof(expected_buf) - 1;
    g_assert_cmpint(vmc->write_pos, ==, expected_buf_len);
    g_assert_true(memcmp(vmc->write_buf, expected_buf, expected_buf_len) == 0);
    basic_event_loop_quit();
}

static void test_smartcard(TestFixture *fixture, gconstpointer user_data)
{
    SpiceServer *const server = test->server;
    uint8_t *p = vmc->message;

    spice_server_add_interface(server, &vmc->instance.base);

    // add VSC_Init message
    memcpy(p, "\x00\x00\x00\x01\x0a\x0b\x0c\x0d\x00\x00\x00\x00", 12);
    p += 12;
    vmc_emu_add_read_till(vmc, p);

    // find Smartcard channel to connect to
    RedChannel *channel = reds_find_channel(server, SPICE_CHANNEL_SMARTCARD, 0);
    g_assert_nonnull(channel);

    // create dummy RedClient and MainChannelClient
    RedChannelCapabilities caps;
    memset(&caps, 0, sizeof(caps));
    uint32_t common_caps = 1 << SPICE_COMMON_CAP_MINI_HEADER;
    caps.num_common_caps = 1;
    caps.common_caps = spice_memdup(&common_caps, sizeof(common_caps));

    RedClient *client = red_client_new(server, FALSE);
    g_assert_nonnull(client);

    MainChannel *main_channel = main_channel_new(server);
    g_assert_nonnull(main_channel);

    MainChannelClient *mcc;
    mcc = main_channel_link(main_channel, client, create_dummy_stream(server, NULL),
                            0, FALSE, &caps);
    g_assert_nonnull(mcc);
    red_client_set_main(client, mcc);

    // create our testing RedChannelClient
    int client_socket;
    red_channel_connect(channel, client, create_dummy_stream(server, &client_socket),
                        FALSE, &caps);
    red_channel_capabilities_reset(&caps);

    // push data to device
    spice_server_char_device_wakeup(&vmc->instance);

    // push data into channel
    send_ack_sync(client_socket, 1);
    send_data(client_socket, VSC_ReaderAdd);
    send_data(client_socket, VSC_APDU);

    // check data are processed after a short time
    SpiceTimer *watch_timer;
    watch_timer = core->timer_add(check_data, core);
    core->timer_start(watch_timer, 100);

    // start all test
    alarm(10);
    basic_event_loop_mainloop();
    alarm(0);

    // cleanup
    core->timer_remove(watch_timer);
    red_client_destroy(client);
    g_object_unref(main_channel);
    g_object_unref(channel);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add("/server/smartcard", TestFixture, NULL, test_smartcard_setup,
               test_smartcard, test_smartcard_teardown);

    return g_test_run();
}
