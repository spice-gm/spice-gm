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

static SpiceCharDeviceInstance vmc_instance;

// device buffer to read from
static uint8_t message[2048];
// position to read from
static unsigned pos;
// array of limits when the read should return
// the array is defined as [message_sizes_curr, message_sizes_end)
// then the size is reach we move on next one till exausted
static unsigned message_sizes[16];
static unsigned *message_sizes_end, *message_sizes_curr;
static bool device_enabled = false;

static unsigned vmc_write_pos;
static uint8_t vmc_write_buf[2048];

// handle writes to the device
static int vmc_write(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                     SPICE_GNUC_UNUSED const uint8_t *buf,
                     int len)
{
    // just copy into the buffer
    unsigned copy = MIN(sizeof(vmc_write_buf) - vmc_write_pos, len);
    memcpy(vmc_write_buf+vmc_write_pos, buf, copy);
    vmc_write_pos += copy;
    return len;
}

static int vmc_read(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                    uint8_t *buf,
                    int len)
{
    int ret;

    if (pos >= *message_sizes_curr && message_sizes_curr < message_sizes_end) {
        ++message_sizes_curr;
    }
    if (message_sizes_curr >= message_sizes_end || pos >= *message_sizes_curr) {
        return 0;
    }
    ret = MIN(*message_sizes_curr - pos, len);
    memcpy(buf, &message[pos], ret);
    pos += ret;
    // kick off next message read
    // currently Qemu kicks the device so we need to do it manually
    // here. If not all data are read, the device goes into blocking
    // state and we get the wake only when we read from the device
    // again
    if (pos >= *message_sizes_curr) {
        spice_server_char_device_wakeup(&vmc_instance);
    }
    return ret;
}

static void vmc_state(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                      SPICE_GNUC_UNUSED int connected)
{
    device_enabled = !!connected;
}

static SpiceCharDeviceInterface vmc_interface = {
    .base = {
        .type          = SPICE_INTERFACE_CHAR_DEVICE,
        .description   = "test spice virtual channel char device",
        .major_version = SPICE_INTERFACE_CHAR_DEVICE_MAJOR,
        .minor_version = SPICE_INTERFACE_CHAR_DEVICE_MINOR,
    },
    .state              = vmc_state,
    .write              = vmc_write,
    .read               = vmc_read,
};

// this specifically creates a stream device
static SpiceCharDeviceInstance vmc_instance = {
    .subtype = "port",
    .portname = "org.spice-space.stream.0",
};

static uint8_t *add_stream_hdr(uint8_t *p, StreamMsgType type, uint32_t size)
{
    StreamDevHeader hdr = {
        .protocol_version = STREAM_DEVICE_PROTOCOL,
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
discard_server_capabilities(void)
{
    StreamDevHeader hdr;

    if (vmc_write_pos == 0) {
        return;
    }
    g_assert(vmc_write_pos >= sizeof(hdr));

    memcpy(&hdr, vmc_write_buf, sizeof(hdr));
    hdr.type = GUINT16_FROM_LE(hdr.type);
    hdr.size = GUINT32_FROM_LE(hdr.size);
    if (hdr.type == STREAM_TYPE_CAPABILITIES) {
        g_assert_cmpint(hdr.size, <=, vmc_write_pos - sizeof(hdr));
        vmc_write_pos -= hdr.size + sizeof(hdr);
        memmove(vmc_write_buf, vmc_write_buf + hdr.size + sizeof(hdr), vmc_write_pos);
    }
}

// check we have an error message on the write buffer
static void
check_vmc_error_message(void)
{
    StreamDevHeader hdr;

    discard_server_capabilities();

    g_assert_cmpint(vmc_write_pos, >= ,sizeof(hdr));

    memcpy(&hdr, vmc_write_buf, sizeof(hdr));
    g_assert_cmpint(hdr.protocol_version, ==, STREAM_DEVICE_PROTOCOL);
    g_assert_cmpint(GUINT16_FROM_LE(hdr.type), ==, STREAM_TYPE_NOTIFY_ERROR);
    g_assert_cmpint(GUINT32_FROM_LE(hdr.size), <=, vmc_write_pos - sizeof(hdr));
}

static int num_send_data_calls = 0;
static size_t send_data_bytes = 0;

struct StreamChannel {
    RedChannel parent;
};

struct StreamChannelClass {
    RedChannelClass parent_class;
};

G_DEFINE_TYPE(StreamChannel, stream_channel, RED_TYPE_CHANNEL)

static void
stream_channel_init(StreamChannel *channel)
{
}

static void
stream_channel_class_init(StreamChannelClass *klass)
{
}

void stream_channel_change_format(StreamChannel *channel,
                                  const struct StreamMsgFormat *fmt)
{
}

void stream_channel_send_data(StreamChannel *channel,
                              const void *data, size_t size,
                              uint32_t mm_time)
{
    ++num_send_data_calls;
    send_data_bytes += size;
}

void stream_channel_register_start_cb(StreamChannel *channel,
                                      stream_channel_start_proc cb, void *opaque)
{
}

void stream_channel_register_queue_stat_cb(StreamChannel *channel,
                                           stream_channel_queue_stat_proc cb, void *opaque)
{
}

StreamChannel* stream_channel_new(RedsState *server, uint32_t id)
{
    return g_object_new(TYPE_STREAM_CHANNEL,
                        "spice-server", server,
                        "core-interface", reds_get_core_interface(server),
                        "channel-type", SPICE_CHANNEL_DISPLAY,
                        "id", id,
                        "migration-flags", 0,
                        "handle-acks", FALSE,
                        NULL);
}

void stream_channel_reset(StreamChannel *channel)
{
}

static SpiceCoreInterface *core;
static Test *test;
typedef int TestFixture;

static void test_stream_device_setup(TestFixture *fixture, gconstpointer user_data)
{
    g_assert_null(core);
    g_assert_null(test);
    core = basic_event_loop_init();
    g_assert_nonnull(core);
    test = test_new(core);
    g_assert_nonnull(test);

    pos = 0;
    vmc_write_pos = 0;
    message_sizes_curr = message_sizes;
    message_sizes_end = message_sizes;

    num_send_data_calls = 0;
    send_data_bytes = 0;
}

static void test_stream_device_teardown(TestFixture *fixture, gconstpointer user_data)
{
    g_assert_nonnull(core);
    g_assert_nonnull(test);

    test_destroy(test);
    test = NULL;
    basic_event_loop_destroy();
    core = NULL;
}

static void test_kick(void)
{
    vmc_instance.base.sif = &vmc_interface.base;
    spice_server_add_interface(test->server, &vmc_instance.base);

    // we need to open the device and kick the start
    // the alarm is to prevent the program from getting stuck
    alarm(5);
    spice_server_port_event(&vmc_instance, SPICE_PORT_EVENT_OPENED);
    spice_server_char_device_wakeup(&vmc_instance);
    alarm(0);
}

static void test_stream_device(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = message;

    for (int test_num=0; test_num < 2; ++test_num) {
        pos = 0;
        vmc_write_pos = 0;
        message_sizes_curr = message_sizes;
        message_sizes_end = message_sizes;

        // add some messages into device buffer
        // here we are testing the device is reading at least two
        // consecutive format messages
        // first message part has 2 extra bytes to check for header split
        p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
        *message_sizes_end = p - message + 2;
        ++message_sizes_end;

        p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_VP9);

        // this split the second format in half
        *message_sizes_end = p - message - 4;
        ++message_sizes_end;

        *message_sizes_end = p - message;
        ++message_sizes_end;

        // add a message to stop data to be read
        p = add_stream_hdr(p, STREAM_TYPE_INVALID, 0);
        *message_sizes_end = p - message;
        ++message_sizes_end;

        // this message should not be read
        p = add_stream_hdr(p, STREAM_TYPE_INVALID, 0);
        *message_sizes_end = p - message;
        ++message_sizes_end;

        vmc_instance.base.sif = &vmc_interface.base;
        spice_server_add_interface(test->server, &vmc_instance.base);

        // device should not have read data before we open it
        spice_server_char_device_wakeup(&vmc_instance);
        g_assert_cmpint(pos, ==, 0);

        g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Stream device received invalid message: Invalid message type");

        // we need to open the device and kick the start
        spice_server_port_event(&vmc_instance, SPICE_PORT_EVENT_OPENED);
        spice_server_char_device_wakeup(&vmc_instance);
        spice_server_port_event(&vmc_instance, SPICE_PORT_EVENT_CLOSED);

        // make sure first 3 parts are read completely
        g_assert(message_sizes_curr - message_sizes >= 3);
        // make sure the device readed all or that device was
        // disabled, we need this to make sure that device will be in
        // sync when opened again
        g_assert(message_sizes_curr - message_sizes == 5 || !device_enabled);

        check_vmc_error_message();
        spice_server_remove_interface(&vmc_instance.base);
    }
}

// check if sending a partial message causes issues
static void test_stream_device_unfinished(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = message;

    // this long and not finished message should not cause an infinite loop
    p = add_stream_hdr(p, STREAM_TYPE_DATA, 100000);
    *message_sizes_end = p - message;
    ++message_sizes_end;

    test_kick();

    // we should have read all data
    g_assert(message_sizes_curr - message_sizes == 1);

    // we should have no data from the device
    discard_server_capabilities();
    g_assert_cmpint(vmc_write_pos, ==, 0);
}

// check if sending multiple messages cause stall
static void test_stream_device_multiple(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = message;

    // add some messages into device buffer
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    *message_sizes_end = p - message;
    ++message_sizes_end;

    test_kick();

    // we should have read all data
    g_assert(message_sizes_curr - message_sizes == 1);
}

// check if data message consume even following message
static void test_stream_device_format_after_data(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = message;

    // add some messages into device buffer
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    p = add_stream_hdr(p, STREAM_TYPE_DATA, 5);
    memcpy(p, "hello", 5);
    p += 5;
    p = add_stream_hdr(p, STREAM_TYPE_INVALID, 0);
    *message_sizes_end = p - message;
    ++message_sizes_end;

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Stream device received invalid message: Invalid message type");

    test_kick();

    // we should read all data
    g_assert(message_sizes_curr - message_sizes == 1);

    // we should have an error back
    check_vmc_error_message();
}

// check empty message
static void test_stream_device_empty(TestFixture *fixture, gconstpointer user_data)
{
    const StreamMsgType msg_type = (StreamMsgType) GPOINTER_TO_INT(user_data);
    uint8_t *p = message;

    // add some messages into device buffer
    p = add_stream_hdr(p, msg_type, 0);
    *message_sizes_end = p - message;
    ++message_sizes_end;
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    *message_sizes_end = p - message;
    ++message_sizes_end;
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    *message_sizes_end = p - message;
    ++message_sizes_end;

    test_kick();

    // we should read all data
    g_assert(message_sizes_curr - message_sizes == 3);

    // we should have no data from the device
    discard_server_capabilities();
    g_assert_cmpint(vmc_write_pos, ==, 0);
}

// check that server refuse huge data messages
static void test_stream_device_huge_data(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = message;

    // add some messages into device buffer
    p = add_stream_hdr(p, STREAM_TYPE_DATA, 33 * 1024 * 1024);
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    *message_sizes_end = p - message;
    ++message_sizes_end;

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Stream device received invalid message: STREAM_DATA too large");

    test_kick();

    // we should read all data
    g_assert(message_sizes_curr - message_sizes == 1);

    // we should have an error back
    check_vmc_error_message();
}

// check that server send all message
static void test_stream_device_data_message(TestFixture *fixture, gconstpointer user_data)
{
    uint8_t *p = message;

    // add some messages into device buffer
    p = add_format(p, 640, 480, SPICE_VIDEO_CODEC_TYPE_MJPEG);
    p = add_stream_hdr(p, STREAM_TYPE_DATA, 1017);
    for (int i = 0; i < 1017; ++i, ++p) {
        *p = (uint8_t) (i * 123 + 57);
    }
    *message_sizes_end = 51;
    ++message_sizes_end;
    *message_sizes_end = 123;
    ++message_sizes_end;
    *message_sizes_end = 534;
    ++message_sizes_end;
    *message_sizes_end = p - message;
    ++message_sizes_end;

    test_kick();

    // we should read all data
    g_assert(message_sizes_curr - message_sizes == 4);

    // we should have no data from the device
    discard_server_capabilities();
    g_assert_cmpint(vmc_write_pos, ==, 0);

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
    uint8_t *p = message;
    p = add_stream_hdr(p, STREAM_TYPE_DEVICE_DISPLAY_INFO, sizeof(info) + sizeof(address));
    memcpy(p, &info, sizeof(info));
    p += sizeof(info);
    strcpy((char*)p, address);
    p += sizeof(address);

    *message_sizes_end = p - message;
    ++message_sizes_end;

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
             test_stream_device, NULL);
    test_add("/server/stream-device-unfinished",
             test_stream_device_unfinished, NULL);
    test_add("/server/stream-device-multiple",
             test_stream_device_multiple, NULL);
    test_add("/server/stream-device-format-after-data",
             test_stream_device_format_after_data, NULL);
    test_add("/server/stream-device-empty-capabilities",
             test_stream_device_empty, GINT_TO_POINTER(STREAM_TYPE_CAPABILITIES));
    test_add("/server/stream-device-empty-data",
             test_stream_device_empty, GINT_TO_POINTER(STREAM_TYPE_DATA));
    test_add("/server/stream-device-huge-data",
             test_stream_device_huge_data, NULL);
    test_add("/server/stream-device-data-message",
             test_stream_device_data_message, NULL);
    test_add("/server/display-info", test_display_info, NULL);

    return g_test_run();
}
