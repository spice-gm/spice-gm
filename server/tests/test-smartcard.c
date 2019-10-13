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
static int client_socket = -1;
// buffer when data from channel are stored
static SpiceBuffer channel_buf;
// expected buffer in channel
static SpiceBuffer channel_expected;
// expected buffer in device
static SpiceBuffer device_expected;
static SpiceWatch *watch;

static void next_test(void);

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

static void send_data(int socket, uint32_t type, uint32_t reader_id)
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
    msg.vheader.reader_id = GUINT32_TO_LE(reader_id);
    msg.vheader.length = GUINT32_TO_LE(6);
    strcpy(msg.data, "hello");

    g_assert_cmpint(socket_write(socket, &msg.type, sizeof(msg)-4), ==, sizeof(msg)-4);
}

static void check_data(VmcEmu *vmc)
{
    g_assert_cmpint(device_expected.offset, !=, 0);
    if (vmc->write_pos < device_expected.offset) {
        return;
    }
    g_assert_cmpint(vmc->write_pos, ==, device_expected.offset);
    g_assert_true(memcmp(vmc->write_buf, device_expected.buffer, device_expected.offset) == 0);
    vmc->write_pos = 0;

    next_test();
}

static void data_from_channel(int fd, int event, void *opaque)
{
    uint8_t buf[128];
    ssize_t ret = socket_read(fd, buf, sizeof(buf));
    if (ret <= 0) {
        g_assert(ret == 0 || errno == EAGAIN || errno == EINTR);
        if (ret == 0) {
            g_warning("TEST: connection closed");
            core->watch_remove(watch);
            watch = NULL;
            next_test();
        }
        return;
    }
    spice_buffer_append(&channel_buf, buf, ret);

    g_assert_cmpint(channel_expected.offset, !=, 0);
    if (channel_buf.offset < channel_expected.offset) {
        return;
    }
    g_assert_true(memcmp(channel_buf.buffer, channel_expected.buffer, channel_expected.offset) == 0);
    spice_buffer_remove(&channel_buf, channel_expected.offset);

    next_test();
}

static void next_test(void)
{
    static int test_num;

    test_num++;
    printf("Executing subtest %d\n", test_num);

    spice_buffer_reset(&channel_expected);
    spice_buffer_reset(&device_expected);

    switch (test_num) {
    // First test, send some message to channel expecting a reply
    // for each message we are sending
    case 1: {
        static const char expected_buf[] =
            // forwarded ReaderAdd message, note that payload is stripped
            "\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00\x00"
            // forwarded APDU message
            "\x00\x00\x00\x07\x00\x00\x00\x00\x00\x00\x00\x06\x68\x65\x6c\x6c\x6f\x00"
            "\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00\x00";
        spice_buffer_append(&device_expected, expected_buf, sizeof(expected_buf) - 1);

        send_data(client_socket, VSC_ReaderAdd, 0);
        send_data(client_socket, VSC_APDU, 0);
        send_data(client_socket, VSC_ReaderRemove, 0);
        } break;
    // Second test, send an init and remove a reader that is not present,
    // we expect an error for the removal (the Init is ignored)
    case 2: {
        static const char expected_buf[] =
            // forwarded Error message
            "\x65\x00\x10\x00\x00\x00"
            "\x02\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00";
        spice_buffer_append(&channel_expected, expected_buf, sizeof(expected_buf) - 1);

        // Init message, ignored
        send_data(client_socket, VSC_Init, 0);
        // remove again, this will trigger an error
        send_data(client_socket, VSC_ReaderRemove, 0);
        } break;
    // Third test, APDU messages from device are forwarded to the channel.
    // We split the header and payload of the first message to check device code can handle it.
    // The second message is send inside a block with the end of the first to trigger
    // an hard path in the device code
    case 3: {
        static const char expected_buf[] =
            // forwarded APDU message
            "\x65\x00\x12\x00\x00\x00"
            "\x07\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00" "foobaz"
            "\x65\x00\x12\x00\x00\x00"
            "\x07\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00" "foobar";
        spice_buffer_append(&channel_expected, expected_buf, sizeof(expected_buf) - 1);

        vmc_emu_reset(vmc);
        // data from device
        uint8_t *p = vmc->message;

        // add VSC_APDU message
        memcpy(p, "\x00\x00\x00\x07\x00\x00\x00\x00\x00\x00\x00\x06" "foobaz", 18);
        p += 18;
        memcpy(p, "\x00\x00\x00\x07\x00\x00\x00\x00\x00\x00\x00\x06" "foobar", 18);
        p += 18;
        vmc_emu_add_read_till(vmc, vmc->message + 8);
        vmc_emu_add_read_till(vmc, vmc->message + 14);
        vmc_emu_add_read_till(vmc, p);

        spice_server_char_device_wakeup(&vmc->instance);
        } break;
    // Fourth test, we should get back an error if client tried to remove
    // a not existing reader
    case 4: {
        static const char expected_buf[] =
            // forwarded Error message
            "\x65\x00\x10\x00\x00\x00"
            "\x02\x00\x00\x00\x05\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00";
        spice_buffer_append(&channel_expected, expected_buf, sizeof(expected_buf) - 1);

        // remove invalid, this will trigger an error
        send_data(client_socket, VSC_ReaderRemove, 5);
        } break;
    // Fifth test, similar to previous but using an huge reader_id field to trigger
    // possible buffer overflow
    case 5: {
        static const char expected_buf[] =
            // forwarded Error message
            "\x65\x00\x10\x00\x00\x00"
            "\x02\x00\x00\x00\x05\x01\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00";
        spice_buffer_append(&channel_expected, expected_buf, sizeof(expected_buf) - 1);

        // remove invalid and huge, this will trigger an error, should not crash
        send_data(client_socket, VSC_ReaderRemove, 261);
        } break;
    // Sixth test, send an invalid message from client, a log is triggered
    // but channel continues to work
    case 6: {
        static const char expected_buf[] =
            // forwarded APDU message
            "\x00\x00\x00\x07\x00\x00\x00\x00\x00\x00\x00\x06\x68\x65\x6c\x6c\x6f\x00";
        spice_buffer_append(&device_expected, expected_buf, sizeof(expected_buf) - 1);

        g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                              "*ERROR: unexpected message on smartcard channel*");

        // invalid message type, should log a warning
        send_data(client_socket, 0xabcd, 0);
        // APDU just to get an event
        send_data(client_socket, VSC_APDU, 0);
        } break;
    // Seventh test, an Error message from device are forwarded to the channel.
    // Note that the header is in big endian order while the error from device
    // is in little endian order. This seems weird but it's correct with the
    // current libcacard implementation which just send error as host order
    case 7: {
        g_test_assert_expected_messages();

        static const char expected_buf[] =
            // forwarded Error message
            "\x65\x00\x10\x00\x00\x00"
            "\x02\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x0a\x0b\x0c\x0d";
        spice_buffer_append(&channel_expected, expected_buf, sizeof(expected_buf) - 1);

        vmc_emu_reset(vmc);
        // data from device
        uint8_t *p = vmc->message;

        // add Error message
        memcpy(p, "\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x04\x0a\x0b\x0c\x0d", 16);
        p += 16;
        vmc_emu_add_read_till(vmc, p);

        spice_server_char_device_wakeup(&vmc->instance);
        } break;
    // Eighth test, a message with invalid reader ID from device caused the channel
    // to be closed
    case 8: {
        g_test_assert_expected_messages();

        g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                              "*ERROR: received message for non existing reader*");
        g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                              "*TEST: connection closed*");

        send_data(client_socket, VSC_APDU, 0xabcd);
        } break;
    case 9:
        g_test_assert_expected_messages();
        basic_event_loop_quit();
        break;
    default:
        abort();
    }
}

static void test_smartcard(TestFixture *fixture, gconstpointer user_data)
{
    SpiceServer *const server = test->server;
    uint8_t *p = vmc->message;

    spice_server_add_interface(server, &vmc->instance.base);

    // add VSC_Init message
    memcpy(p, "\x00\x00\x00\x01\x0a\x0b\x0c\x0d\x00\x00\x00\x00", 12);
    p += 12;
    vmc_emu_add_read_till(vmc, vmc->message + 2); // check header is decoded correctly when split
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

    // create our testing RedChannelClient
    red_channel_connect(channel, client, create_dummy_stream(server, &client_socket),
                        FALSE, &caps);
    red_channel_capabilities_reset(&caps);

    // push data to device
    spice_server_char_device_wakeup(&vmc->instance);

    // push data into channel
    send_ack_sync(client_socket, 1);

    // check data are processed
    watch = core->watch_add(client_socket, SPICE_WATCH_EVENT_READ, data_from_channel, NULL);
    vmc->data_written_cb = check_data;

    // start all test
    alarm(10);
    next_test();
    basic_event_loop_mainloop();
    alarm(0);

    // cleanup
    if (watch) {
        core->watch_remove(watch);
    }
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
