/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2017 Red Hat, Inc.

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
 * Test streaming device
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
#include "stream-channel.h"
#include "reds.h"
#include "win-alarm.h"
#include "vmc-emu.h"

static VmcEmu *vmc;

static uint8_t *add_stream_hdr(uint8_t *p, StreamMsgType type, uint32_t size)
{
    StreamDevHeader hdr = {
        .protocol_version = STREAM_DEVICE_PROTOCOL,
        .padding = 0,
        .type = GUINT16_TO_LE(type),
        .size = GUINT32_TO_LE(size),
    };

    memcpy(p, &hdr, sizeof(hdr));
    return p + sizeof(hdr);
}

static uint8_t *add_format(uint8_t *p, uint32_t w, uint32_t h, SpiceVideoCodecType codec)
{
    StreamMsgFormat fmt = {
        .width = GUINT32_TO_LE(w),
        .height = GUINT32_TO_LE(h),
        .codec = codec,
    };

    p = add_stream_hdr(p, STREAM_TYPE_FORMAT, sizeof(fmt));
    memcpy(p, &fmt, sizeof(fmt));
    return p + sizeof(fmt);
}

/* currently we don't care about possible capabilities sent so discard them
 * from server reply */
static void
discard_server_capabilities()
{
    StreamDevHeader hdr;

    if (vmc->write_pos == 0) {
        return;
    }
    g_assert(vmc->write_pos >= sizeof(hdr));

    memcpy(&hdr, vmc->write_buf, sizeof(hdr));
    hdr.type = GUINT16_FROM_LE(hdr.type);
    hdr.size = GUINT32_FROM_LE(hdr.size);
    if (hdr.type == STREAM_TYPE_CAPABILITIES) {
        g_assert_cmpint(hdr.size, <=, vmc->write_pos - sizeof(hdr));
        vmc->write_pos -= hdr.size + sizeof(hdr);
        memmove(vmc->write_buf, vmc->write_buf + hdr.size + sizeof(hdr), vmc->write_pos);
    }
}

// check we have an error message on the write buffer
static void
check_vmc_error_message()
{
    StreamDevHeader hdr;

    discard_server_capabilities();

    g_assert_cmpint(vmc->write_pos, >= ,sizeof(hdr));

    memcpy(&hdr, vmc->write_buf, sizeof(hdr));
    g_assert_cmpint(hdr.protocol_version, ==, STREAM_DEVICE_PROTOCOL);
    g_assert_cmpint(GUINT16_FROM_LE(hdr.type), ==, STREAM_TYPE_NOTIFY_ERROR);
    g_assert_cmpint(GUINT32_FROM_LE(hdr.size), <=, vmc->write_pos - sizeof(hdr));
}

static int num_send_data_calls = 0;
static size_t send_data_bytes = 0;

StreamChannel::StreamChannel(RedsState *reds, uint32_t id):
    RedChannel(reds, SPICE_CHANNEL_DISPLAY, id, RedChannel::HandleAcks)
{
    reds_register_channel(reds, this);
}

void
StreamChannel::change_format(const StreamMsgFormat *fmt)
{
}

void
StreamChannel::send_data(const void *data, size_t size, uint32_t mm_time)
{
    ++num_send_data_calls;
    send_data_bytes += size;
}

void
StreamChannel::register_start_cb(stream_channel_start_proc cb, void *opaque)
{
}

void
StreamChannel::register_queue_stat_cb(stream_channel_queue_stat_proc cb, void *opaque)
{
}

red::shared_ptr<StreamChannel> stream_channel_new(RedsState *server, uint32_t id)
{
    return red::make_shared<StreamChannel>(server, id);
}

void
StreamChannel::reset()
{
}

void StreamChannel::on_connect(RedClient *red_client, RedStream *stream,
                               int migration, RedChannelCapabilities *caps)
{
}

static SpiceCoreInterface *core;
static Test *test;
using TestFixture = int;

static void test_stream_device_setup(TestFixture *fixture, gconstpointer user_data)
{
    g_assert_null(core);
    g_assert_null(test);
    g_assert_null(vmc);
    core = basic_event_loop_init();
    g_assert_nonnull(core);
    test = test_new(core);
    g_assert_nonnull(test);
    vmc = vmc_emu_new("port", "org.spice-space.stream.0");
    g_assert_nonnull(vmc);

    num_send_data_calls = 0;
    send_data_bytes = 0;
}

static void test_stream_device_teardown(TestFixture *fixture, gconstpointer user_data)
{
    g_assert_nonnull(core);
    g_assert_nonnull(test);

    vmc_emu_destroy(vmc);
    vmc = nullptr;
    test_destroy(test);
    test = nullptr;
    basic_event_loop_destroy();
    core = nullptr;
}

static void test_kick()
{
    spice_server_add_interface(test->server, &vmc->instance.base);

    // we need to open the device and kick the start
    // the alarm is to prevent the program from getting stuck
    alarm(5);
    spice_server_port_event(&vmc->instance, SPICE_PORT_EVENT_OPENED);
    spice_server_char_device_wakeup(&vmc->instance);
    alarm(0);
}

static void test_stream_device(TestFixture *fixture, gconstpointer user_data)
{
    for (int test_num=0; test_num < 2; ++test_num) {
        vmc_emu_reset(vmc);
        uint8_t *p = vmc->message;

        // add some messages into device buffer
        // here we are testing the device is reading at least two
        // consecutive format messages
        // first message part has 2 extra bytes to check for header split
        p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
        vmc_emu_add_read_till(vmc, p + 2);

        p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_VP9);

        // this split the second format in half
        vmc_emu_add_read_till(vmc, p - 4);

        vmc_emu_add_read_till(vmc, p);

        // add a message to stop data to be read
        p = add_stream_hdr(p, STREAM_TYPE_INVALID, 0);
        vmc_emu_add_read_till(vmc, p);

        // this message should not be read
        p = add_stream_hdr(p, STREAM_TYPE_INVALID, 0);
        vmc_emu_add_read_till(vmc, p);

        spice_server_add_interface(test->server, &vmc->instance.base);

        // device should not have read data before we open it
        spice_server_char_device_wakeup(&vmc->instance);
        g_assert_cmpint(vmc->pos, ==, 0);

        g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Stream device received invalid message: Invalid message type");

        // we need to open the device and kick the start
        spice_server_port_event(&vmc->instance, SPICE_PORT_EVENT_OPENED);
        spice_server_char_device_wakeup(&vmc->instance);
        spice_server_port_event(&vmc->instance, SPICE_PORT_EVENT_CLOSED);

        // make sure first 3 parts are read completely
        g_assert(vmc->message_sizes_curr - vmc->message_sizes >= 3);
        // make sure the device readed all or that device was
        // disabled, we need this to make sure that device will be in
        // sync when opened again
        g_assert(vmc->message_sizes_curr - vmc->message_sizes == 5 || !vmc->device_enabled);

        check_vmc_error_message();
        spice_server_remove_interface(&vmc->instance.base);
    }
}

// check if sending a partial message causes issues
static void test_stream_device_unfinished(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = vmc->message;

    // this long and not finished message should not cause an infinite loop
    p = add_stream_hdr(p, STREAM_TYPE_DATA, 100000);
    vmc_emu_add_read_till(vmc, p);

    test_kick();

    // we should have read all data
    g_assert(vmc->message_sizes_curr - vmc->message_sizes == 1);

    // we should have no data from the device
    discard_server_capabilities();
    g_assert_cmpint(vmc->write_pos, ==, 0);
}

// check if sending multiple messages cause stall
static void test_stream_device_multiple(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = vmc->message;

    // add some messages into device buffer
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    vmc_emu_add_read_till(vmc, p);

    test_kick();

    // we should have read all data
    g_assert(vmc->message_sizes_curr - vmc->message_sizes == 1);
}

// check if data message consume even following message
static void test_stream_device_format_after_data(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = vmc->message;

    // add some messages into device buffer
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    p = add_stream_hdr(p, STREAM_TYPE_DATA, 5);
    memcpy(p, "hello", 5);
    p += 5;
    p = add_stream_hdr(p, STREAM_TYPE_INVALID, 0);
    vmc_emu_add_read_till(vmc, p);

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Stream device received invalid message: Invalid message type");

    test_kick();

    // we should read all data
    g_assert(vmc->message_sizes_curr - vmc->message_sizes == 1);

    // we should have an error back
    check_vmc_error_message();
}

// check empty message
static void test_stream_device_empty(TestFixture *fixture, gconstpointer user_data)
{
    const auto msg_type = (StreamMsgType) GPOINTER_TO_INT(user_data);
    uint8_t *p = vmc->message;

    // add some messages into device buffer
    p = add_stream_hdr(p, msg_type, 0);
    vmc_emu_add_read_till(vmc, p);
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    vmc_emu_add_read_till(vmc, p);
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    vmc_emu_add_read_till(vmc, p);

    test_kick();

    // we should read all data
    g_assert(vmc->message_sizes_curr - vmc->message_sizes == 3);

    // we should have no data from the device
    discard_server_capabilities();
    g_assert_cmpint(vmc->write_pos, ==, 0);
}

// check that server refuse huge data messages
static void test_stream_device_huge_data(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = vmc->message;

    // add some messages into device buffer
    p = add_stream_hdr(p, STREAM_TYPE_DATA, 33 * 1024 * 1024);
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    vmc_emu_add_read_till(vmc, p);

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Stream device received invalid message: STREAM_DATA too large");

    test_kick();

    // we should read all data
    g_assert(vmc->message_sizes_curr - vmc->message_sizes == 1);

    // we should have an error back
    check_vmc_error_message();
}

// check that server send all message
static void test_stream_device_data_message(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = vmc->message;

    // add some messages into device buffer
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    p = add_stream_hdr(p, STREAM_TYPE_DATA, 1017);
    for (int i = 0; i < 1017; ++i, ++p) {
        *p = (uint8_t) (i * 123 + 57);
    }
    vmc_emu_add_read_till(vmc, vmc->message + 51);
    vmc_emu_add_read_till(vmc, vmc->message + 123);
    vmc_emu_add_read_till(vmc, vmc->message + 534);
    vmc_emu_add_read_till(vmc, p);

    test_kick();

    // we should read all data
    g_assert(vmc->message_sizes_curr - vmc->message_sizes == 4);

    // we should have no data from the device
    discard_server_capabilities();
    g_assert_cmpint(vmc->write_pos, ==, 0);

    // make sure data were collapsed in a single message
    g_assert_cmpint(num_send_data_calls, ==, 1);
    g_assert_cmpint(send_data_bytes, ==, 1017);
}

static void test_display_info(TestFixture *fixture, gconstpointer user_data)
{
    // initialize a QXL interface. This must be done before receiving the display info message from
    // the stream
    test_add_display_interface(test);
    // qxl device supports 2 monitors
    spice_qxl_set_device_info(&test->qxl_instance, "pci/0/1.2", 0, 2);

    // craft a message from the mock stream device that provides display info to the server for the
    // given stream
    static const char address[] = "pci/a/b.cde";
    StreamMsgDeviceDisplayInfo info = {
        .stream_id = GUINT32_TO_LE(0x01020304),
        .device_display_id = GUINT32_TO_LE(0x0a0b0c0d),
        .device_address_len = GUINT32_TO_LE(sizeof(address)),
    };
    uint8_t *p = vmc->message;
    p = add_stream_hdr(p, STREAM_TYPE_DEVICE_DISPLAY_INFO, sizeof(info) + sizeof(address));
    memcpy(p, &info, sizeof(info));
    p += sizeof(info);
    strcpy((char*)p, address);
    p += sizeof(address);

    vmc_emu_add_read_till(vmc, p);

    // parse the simulated display info message from the stream device so the server now has display
    // info for the mock stream device
    test_kick();

    // build the buffer to send to the agent for display information
    SpiceMarshaller *m = spice_marshaller_new();
    reds_marshall_device_display_info(test->server, m);
    int to_free;
    size_t buf_len;
    uint8_t *buf = spice_marshaller_linearize(m, 0, &buf_len, &to_free);

    // check output buffer. The message that we send to the vdagent should combine display info for
    // the stream device that we crafted above and the qxl device.
    static const uint8_t expected_buffer[] = {
        /* device count */        3,  0,  0,  0,

        /* channel_id */          0,  0,  0,  0,
        /* monitor_id */          0,  0,  0,  0,
        /* device_display_id */   0,  0,  0,  0,
        /* device_address_len */ 10,  0,  0,  0,
        /* device_address */    'p','c','i','/','0','/','1','.','2',  0,

        /* channel_id */          0,  0,  0,  0,
        /* monitor_id */          1,  0,  0,  0,
        /* device_display_id */   1,  0,  0,  0,
        /* device_address_len */ 10,  0,  0,  0,
        /* device_address */    'p','c', 'i','/','0','/','1','.','2',  0,

        /* channel_id */          1,  0,  0,  0,
        /* monitor_id */          4,  3,  2,  1,
        /* device_display_id */  13, 12, 11, 10,
        /* device_address_len */ 12,  0,  0,  0,
        /* device_address */    'p','c','i','/','a','/','b','.','c','d','e',  0
    };
    g_assert_cmpint(buf_len, ==, sizeof(expected_buffer));
    g_assert_true(memcmp(buf, expected_buffer, buf_len) == 0);

    if (to_free) {
        free(buf);
    }
    spice_marshaller_destroy(m);
}

static void test_add(const char *name, void (*func)(TestFixture *, gconstpointer), gconstpointer arg)
{
    g_test_add(name, TestFixture, arg, test_stream_device_setup, func, test_stream_device_teardown);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    test_add("/server/stream-device",
             test_stream_device, nullptr);
    test_add("/server/stream-device-unfinished",
             test_stream_device_unfinished, nullptr);
    test_add("/server/stream-device-multiple",
             test_stream_device_multiple, nullptr);
    test_add("/server/stream-device-format-after-data",
             test_stream_device_format_after_data, nullptr);
    test_add("/server/stream-device-empty-capabilities",
             test_stream_device_empty, GINT_TO_POINTER(STREAM_TYPE_CAPABILITIES));
    test_add("/server/stream-device-empty-data",
             test_stream_device_empty, GINT_TO_POINTER(STREAM_TYPE_DATA));
    test_add("/server/stream-device-huge-data",
             test_stream_device_huge_data, nullptr);
    test_add("/server/stream-device-data-message",
             test_stream_device_data_message, nullptr);
    test_add("/server/display-info", test_display_info, nullptr);

    return g_test_run();
}
