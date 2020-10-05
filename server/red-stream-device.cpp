/* spice-server character device to handle a video stream

   Copyright (C) 2017-2018 Red Hat, Inc.

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

#include <common/recorder.h>

#include "red-stream-device.h"

#include "stream-channel.h"
#include "cursor-channel.h"
#include "reds.h"

static void char_device_set_state(RedCharDevice *char_dev, int state);

RECORDER(stream_device_data, 32, "Stream device data packet");

void
StreamDevice::close_timer_func(StreamDevice *dev)
{
    if (dev->opened && dev->has_error) {
        char_device_set_state(dev, 0);
    }
}

static void
fill_dev_hdr(StreamDevHeader *hdr, StreamMsgType msg_type, uint32_t msg_size)
{
    hdr->protocol_version = STREAM_DEVICE_PROTOCOL;
    hdr->padding = 0;
    hdr->type = GUINT16_TO_LE(msg_type);
    hdr->size = GUINT32_TO_LE(msg_size);
}

bool
StreamDevice::partial_read()
{
    int n;
    bool handled = false;

    // in order to get in sync every time we open the device we need to discard data here.
    // Qemu keeps a buffer of data which is used only during spice_server_char_device_wakeup
    // from Qemu
    if (G_UNLIKELY(has_error)) {
        uint8_t buf[16 * 1024];
        while (read(buf, sizeof(buf)) > 0) {
            continue;
        }

        // This code is a workaround for a Qemu bug, see patch
        // "stream-device: Workaround Qemu bug closing device".
        // As calling sif->state here can cause a crash, schedule
        // a timer and do the call in it. Remove this code when
        // we are sure all Qemu versions have been patched.
        RedsState *reds = get_server();
        if (!close_timer) {
            close_timer = reds_core_timer_add(reds, close_timer_func, this);
        }
        red_timer_start(close_timer, 0);
        return false;
    }

    if (flow_stopped || !stream_channel) {
        return false;
    }

    /* read header */
    while (hdr_pos < sizeof(hdr)) {
        n = read((uint8_t *) &hdr + hdr_pos, sizeof(hdr) - hdr_pos);
        if (n <= 0) {
            return false;
        }
        hdr_pos += n;
        if (hdr_pos >= sizeof(hdr)) {
            hdr.type = GUINT16_FROM_LE(hdr.type);
            hdr.size = GUINT32_FROM_LE(hdr.size);
            msg_pos = 0;
        }
    }

    switch ((StreamMsgType) hdr.type) {
    case STREAM_TYPE_FORMAT:
        if (hdr.size != sizeof(StreamMsgFormat)) {
            handled = handle_msg_invalid("Wrong size for StreamMsgFormat");
        } else {
            handled = handle_msg_format();
        }
        break;
    case STREAM_TYPE_DEVICE_DISPLAY_INFO:
        if (hdr.size > sizeof(StreamMsgDeviceDisplayInfo) + MAX_DEVICE_ADDRESS_LEN) {
            handled = handle_msg_invalid("StreamMsgDeviceDisplayInfo too large");
        } else {
            handled = handle_msg_device_display_info();
        }
        break;
    case STREAM_TYPE_DATA:
        if (hdr.size > 32*1024*1024) {
            handled = handle_msg_invalid("STREAM_DATA too large");
        } else {
            handled = handle_msg_data();
        }
        break;
    case STREAM_TYPE_CURSOR_SET:
        handled = handle_msg_cursor_set();
        break;
    case STREAM_TYPE_CURSOR_MOVE:
        if (hdr.size != sizeof(StreamMsgCursorMove)) {
            handled = handle_msg_invalid("Wrong size for StreamMsgCursorMove");
        } else {
            handled = handle_msg_cursor_move();
        }
        break;
    case STREAM_TYPE_CAPABILITIES:
        handled = handle_msg_capabilities();
        break;
    default:
        handled = handle_msg_invalid("Invalid message type");
        break;
    }

    /* current message has been handled, so reset state and get ready to parse
     * the next message */
    if (handled) {
        hdr_pos = 0;

        // Reallocate message buffer to the minimum.
        // Currently the only message that requires resizing is the cursor shape,
        // which is not expected to be sent so often.
        if (msg_len > sizeof(*msg)) {
            msg = (StreamDevice::AllMessages*) g_realloc(msg, sizeof(*msg));
            msg_len = sizeof(*msg);
        }
    }

    // Qemu put the device on blocking state if we don't read all data
    // so schedule another read.
    // We arrive here if we processed that entire message or we
    // got an error, try to read another message or discard the
    // wrong data
    return handled || has_error;
}

RedPipeItemPtr StreamDevice::read_one_msg_from_device()
{
    while (partial_read()) {
        continue;
    }
    return RedPipeItemPtr();
}

bool
StreamDevice::handle_msg_invalid(const char *error_msg)
{
    static const char default_error_msg[] = "Protocol error";

    spice_extra_assert(hdr_pos >= sizeof(StreamDevHeader));

    if (!error_msg) {
        error_msg = default_error_msg;
    }

    g_warning("Stream device received invalid message: %s", error_msg);

    int msg_size = sizeof(StreamMsgNotifyError) + strlen(error_msg) + 1;
    int total_size = sizeof(StreamDevHeader) + msg_size;

    RedCharDeviceWriteBuffer *buf =
        write_buffer_get_server(total_size, false);
    buf->buf_used = total_size;

    auto const header = (StreamDevHeader *)buf->buf;
    fill_dev_hdr(header, STREAM_TYPE_NOTIFY_ERROR, msg_size);

    auto const error = (StreamMsgNotifyError *)(header+1);
    error->error_code = GUINT32_TO_LE(0);
    strcpy((char *) error->msg, error_msg);

    write_buffer_add(buf);

    has_error = true;
    return false;
}

bool
StreamDevice::handle_msg_format()
{
    spice_extra_assert(hdr_pos >= sizeof(StreamDevHeader));
    spice_extra_assert(hdr.type == STREAM_TYPE_FORMAT);

    int n = read(msg->buf + msg_pos, sizeof(StreamMsgFormat) - msg_pos);
    if (n < 0) {
        return handle_msg_invalid(nullptr);
    }

    msg_pos += n;

    if (msg_pos < sizeof(StreamMsgFormat)) {
        return false;
    }

    msg->format.width = GUINT32_FROM_LE(msg->format.width);
    msg->format.height = GUINT32_FROM_LE(msg->format.height);
    stream_channel->change_format(&msg->format);
    return true;
}

bool
StreamDevice::handle_msg_device_display_info()
{
    spice_extra_assert(hdr_pos >= sizeof(StreamDevHeader));
    spice_extra_assert(hdr.type == STREAM_TYPE_DEVICE_DISPLAY_INFO);

    if (msg_len < hdr.size) {
        msg = (StreamDevice::AllMessages*) g_realloc(msg, hdr.size);
        msg_len = hdr.size;
    }

    /* read from device */
    ssize_t n = read(msg->buf + msg_pos, hdr.size - msg_pos);
    if (n <= 0) {
        return msg_pos == hdr.size;
    }

    msg_pos += n;
    if (msg_pos != hdr.size) { /* some bytes are still missing */
        return false;
    }

    StreamMsgDeviceDisplayInfo *display_info_msg = &msg->device_display_info;

    size_t device_address_len = GUINT32_FROM_LE(display_info_msg->device_address_len);
    if (device_address_len > MAX_DEVICE_ADDRESS_LEN) {
        g_warning("Received a device address longer than %u (%zu), "
                  "will be truncated!", MAX_DEVICE_ADDRESS_LEN, device_address_len);
        device_address_len = sizeof(device_display_info.device_address);
    }

    if (device_address_len == 0) {
        g_warning("Zero length device_address in  DeviceDisplayInfo message, ignoring.");
        return true;
    }

    if (display_info_msg->device_address + device_address_len > (uint8_t*) msg + hdr.size) {
        g_warning("Malformed DeviceDisplayInfo message, device_address length (%zu) "
                  "goes beyond the end of the message, ignoring.", device_address_len);
        return true;
    }

    memcpy(device_display_info.device_address,
           (char*) display_info_msg->device_address,
           device_address_len);

    // make sure the string is terminated
    device_display_info.device_address[device_address_len - 1] = '\0';

    device_display_info.stream_id = GUINT32_FROM_LE(display_info_msg->stream_id);
    device_display_info.device_display_id = GUINT32_FROM_LE(display_info_msg->device_display_id);

    g_debug("Received DeviceDisplayInfo from the streaming agent: stream_id %u, "
            "device_address %s, device_display_id %u",
            device_display_info.stream_id,
            device_display_info.device_address,
            device_display_info.device_display_id);

    reds_send_device_display_info(get_server());

    return true;
}

bool
StreamDevice::handle_msg_capabilities()
{
    spice_extra_assert(hdr_pos >= sizeof(StreamDevHeader));
    spice_extra_assert(hdr.type == STREAM_TYPE_CAPABILITIES);

    if (hdr.size > STREAM_MSG_CAPABILITIES_MAX_BYTES) {
        return handle_msg_invalid("Wrong size for StreamMsgCapabilities");
    }

    int n = read(msg->buf + msg_pos, hdr.size - msg_pos);
    if (n < 0) {
        return handle_msg_invalid(nullptr);
    }

    msg_pos += n;

    if (msg_pos < hdr.size) {
        return false;
    }

    // copy only capabilities we know about
    memset(guest_capabilities, 0, sizeof(guest_capabilities));
    memcpy(guest_capabilities, msg->capabilities.capabilities,
           MIN(sizeof(guest_capabilities), hdr.size));

    return true;
}

bool
StreamDevice::handle_msg_data()
{
    int n;

    spice_extra_assert(hdr_pos >= sizeof(StreamDevHeader));
    spice_extra_assert(hdr.type == STREAM_TYPE_DATA);

    /* make sure we have a large enough buffer for the whole frame */
    /* ---
     * TODO: Now that we copy partial data into the buffer, for each frame
     * the buffer is allocated and freed (look for g_realloc in
     * partial_read).
     * Probably better to just keep the larger buffer.
     */
    if (msg_pos == 0) {
        frame_mmtime = reds_get_mm_time();
        record(stream_device_data, "Stream data packet size %u mm_time %u",
               hdr.size, frame_mmtime);
        if (msg_len < hdr.size) {
            g_free(msg);
            msg = (StreamDevice::AllMessages*) g_malloc(hdr.size);
            msg_len = hdr.size;
        }
    }

    /* read from device */
    n = read(msg->buf + msg_pos, hdr.size - msg_pos);
    if (n <= 0) {
        return msg_pos == hdr.size;
    }

    msg_pos += n;
    if (msg_pos != hdr.size) { /* some bytes are still missing */
        return false;
    }

    /* The whole frame was read from the device, send it */
    stream_channel->send_data(msg->buf, hdr.size, frame_mmtime);

    return true;
}

/*
 * Returns number of bits required for a pixel of a given cursor type.
 *
 * Take into account mask bits.
 * Returns 0 on error.
 */
static unsigned int
get_cursor_type_bits(unsigned int cursor_type)
{
    switch (cursor_type) {
    case SPICE_CURSOR_TYPE_ALPHA:
        // RGBA
        return 32;
    case SPICE_CURSOR_TYPE_COLOR24:
        // RGB + bitmask
        return 24 + 1;
    case SPICE_CURSOR_TYPE_COLOR32:
        // RGBx + bitmask
        return 32 + 1;
    default:
        return 0;
    }
}

static RedCursorCmd *
stream_msg_cursor_set_to_cursor_cmd(const StreamMsgCursorSet *msg, size_t msg_size)
{
    auto cmd = g_new0(RedCursorCmd, 1);
    cmd->type = QXL_CURSOR_SET;
    cmd->u.set.position.x = 0; // TODO
    cmd->u.set.position.y = 0; // TODO
    cmd->u.set.visible = 1; // TODO
    SpiceCursor *cursor = &cmd->u.set.shape;
    cursor->header.unique = 0;
    cursor->header.type = msg->type;
    cursor->header.width = GUINT16_FROM_LE(msg->width);
    cursor->header.height = GUINT16_FROM_LE(msg->height);
    cursor->header.hot_spot_x = GUINT16_FROM_LE(msg->hot_spot_x);
    cursor->header.hot_spot_y = GUINT16_FROM_LE(msg->hot_spot_y);

    /* Limit cursor size to prevent DoS */
    if (cursor->header.width > STREAM_MSG_CURSOR_SET_MAX_WIDTH ||
        cursor->header.height > STREAM_MSG_CURSOR_SET_MAX_HEIGHT) {
        g_free(cmd);
        return nullptr;
    }

    const unsigned int cursor_bits = get_cursor_type_bits(cursor->header.type);
    if (cursor_bits == 0) {
        g_free(cmd);
        return nullptr;
    }

    /* Check that enough data has been sent for the cursor.
     * Note that these computations can't overflow due to sizes checks
     * above. */
    size_t size_required = cursor->header.width * cursor->header.height;
    size_required = SPICE_ALIGN(size_required * cursor_bits, 8) / 8u;
    if (msg_size < sizeof(StreamMsgCursorSet) + size_required) {
        g_free(cmd);
        return nullptr;
    }
    cursor->data_size = size_required;
    cursor->data = (uint8_t*) g_memdup(msg->data, size_required);
    return cmd;
}

bool
StreamDevice::handle_msg_cursor_set()
{
    // Calculate the maximum size required to send the pixel data for a cursor that is the
    // maximum size using the format that requires the largest number of bits per pixel
    // (SPICE_CURSOR_TYPE_COLOR32 requires 33 bits per pixel, see get_cursor_type_bits())
    const unsigned int max_cursor_set_size =
        sizeof(StreamMsgCursorSet) +
        (STREAM_MSG_CURSOR_SET_MAX_WIDTH * 4 + (STREAM_MSG_CURSOR_SET_MAX_WIDTH + 7)/8)
        * STREAM_MSG_CURSOR_SET_MAX_HEIGHT;

    if (hdr.size < sizeof(StreamMsgCursorSet) || hdr.size > max_cursor_set_size) {
        // we could skip the message but on the other hand the guest
        // is buggy in any case
        return handle_msg_invalid("Cursor size is invalid");
    }

    // read part of the message till we get all
    if (msg_len < hdr.size) {
        msg = (StreamDevice::AllMessages*) g_realloc(msg, hdr.size);
        msg_len = hdr.size;
    }
    int n = read(msg->buf + msg_pos, hdr.size - msg_pos);
    if (n <= 0) {
        return false;
    }
    msg_pos += n;
    if (msg_pos != hdr.size) {
        return false;
    }

    // transform the message to a cursor command and process it
    RedCursorCmd *cmd = stream_msg_cursor_set_to_cursor_cmd(&msg->cursor_set, msg_pos);
    if (!cmd) {
        return handle_msg_invalid(nullptr);
    }
    cursor_channel->process_cmd(cmd);

    return true;
}

bool
StreamDevice::handle_msg_cursor_move()
{
    int n = read(msg->buf + msg_pos, hdr.size - msg_pos);
    if (n <= 0) {
        return false;
    }
    msg_pos += n;
    if (msg_pos != hdr.size) {
        return false;
    }

    StreamMsgCursorMove *move = &msg->cursor_move;
    move->x = GINT32_FROM_LE(move->x);
    move->y = GINT32_FROM_LE(move->y);

    auto cmd = g_new0(RedCursorCmd, 1);
    cmd->type = QXL_CURSOR_MOVE;
    cmd->u.position.x = move->x;
    cmd->u.position.y = move->y;

    cursor_channel->process_cmd(cmd);

    return true;
}

void StreamDevice::remove_client(RedCharDeviceClientOpaque *client)
{
}

void
StreamDevice::stream_start(void *opaque, StreamMsgStartStop *start,
                           StreamChannel *stream_channel G_GNUC_UNUSED)
{
    auto dev = (StreamDevice *) opaque;

    if (!dev->opened) {
        return;
    }

    int msg_size = sizeof(*start) + sizeof(start->codecs[0]) * start->num_codecs;
    int total_size = sizeof(StreamDevHeader) + msg_size;

    RedCharDeviceWriteBuffer *buf =
        dev->write_buffer_get_server(total_size, false);
    buf->buf_used = total_size;

    auto hdr = (StreamDevHeader *)buf->buf;
    fill_dev_hdr(hdr, STREAM_TYPE_START_STOP, msg_size);

    memcpy(&hdr[1], start, msg_size);

    dev->write_buffer_add(buf);
}

void
StreamDevice::stream_queue_stat(void *opaque, const StreamQueueStat *stats G_GNUC_UNUSED,
                                StreamChannel *stream_channel G_GNUC_UNUSED)
{
    auto dev = (StreamDevice *) opaque;

    if (!dev->opened) {
        return;
    }

    // very easy control flow... if any data stop
    // this seems a very small queue but as we use tcp
    // there's already that queue
    if (stats->num_items) {
        dev->flow_stopped = true;
        return;
    }

    if (dev->flow_stopped) {
        dev->flow_stopped = false;
        // TODO resume flow...
        // avoid recursion if we need to call get data from data handling from
        // data handling
        dev->wakeup();
    }
}

red::shared_ptr<StreamDevice>
stream_device_connect(RedsState *reds, SpiceCharDeviceInstance *sin)
{
    SpiceCharDeviceInterface *sif;

    auto dev = red::make_shared<StreamDevice>(reds, sin);

    sif = spice_char_device_get_interface(sin);
    if (sif->state) {
        sif->state(sin, 1);
    }

    return dev;
}

StreamDevice::StreamDevice(RedsState *reds, SpiceCharDeviceInstance *sin):
    RedCharDevice(reds, sin, 0, 0)
{
    msg = (StreamDevice::AllMessages*) g_malloc(sizeof(*msg));
    msg_len = sizeof(*msg);
}

StreamDevice::~StreamDevice()
{
    red_timer_remove(close_timer);

    if (stream_channel) {
        // close all current connections
        stream_channel->destroy();
    }
    if (cursor_channel) {
        // close all current connections
        cursor_channel->destroy();
    }

    g_free(msg);
}

void
StreamDevice::create_channel()
{
    if (stream_channel) {
        return;
    }

    SpiceServer* reds = get_server();
    SpiceCoreInterfaceInternal* core = reds_get_core_interface(reds);

    int id = reds_get_free_channel_id(reds, SPICE_CHANNEL_DISPLAY);
    g_return_if_fail(id >= 0);

    stream_channel = stream_channel_new(reds, id);
    cursor_channel = cursor_channel_new(reds, id, core, nullptr);

    stream_channel->register_start_cb(stream_start, this);
    stream_channel->register_queue_stat_cb(stream_queue_stat, this);
}

void
StreamDevice::reset_channels()
{
    if (stream_channel) {
        stream_channel->reset();
    }
}

static void
char_device_set_state(RedCharDevice *char_dev, int state)
{
    SpiceCharDeviceInstance *sin;
    sin = char_dev->get_device_instance();
    spice_assert(sin != nullptr);

    SpiceCharDeviceInterface *sif = spice_char_device_get_interface(sin);
    if (sif->state) {
        sif->state(sin, state);
    }
}

static void
send_capabilities(RedCharDevice *char_dev)
{
    int msg_size = MAX_GUEST_CAPABILITIES_BYTES;
    int total_size = sizeof(StreamDevHeader) + msg_size;

    RedCharDeviceWriteBuffer *buf =
        char_dev->write_buffer_get_server(total_size, false);
    buf->buf_used = total_size;

    auto const hdr = (StreamDevHeader *)buf->buf;
    fill_dev_hdr(hdr, STREAM_TYPE_CAPABILITIES, msg_size);

    auto const caps = (StreamMsgCapabilities *)(hdr+1);
    memset(caps, 0, msg_size);

    char_dev->write_buffer_add(buf);
}

void
StreamDevice::port_event(uint8_t event)
{
    if (event != SPICE_PORT_EVENT_OPENED && event != SPICE_PORT_EVENT_CLOSED) {
        return;
    }

    // reset device and channel on close/open
    opened = (event == SPICE_PORT_EVENT_OPENED);
    if (opened) {
        create_channel();

        send_capabilities(this);
    }
    hdr_pos = 0;
    msg_pos = 0;
    has_error = false;
    flow_stopped = false;
    reset();
    reset_channels();

    // enable the device again. We re-enable it on close as otherwise we don't want to get a
    // failure when  we try to re-open the device as would happen if we keep it disabled
    char_device_set_state(this, 1);
}

const StreamDeviceDisplayInfo *StreamDevice::get_device_display_info()
{
    return &device_display_info;
}

int32_t StreamDevice::get_stream_channel_id()
{
    if (!stream_channel) {
        return -1;
    }

    return stream_channel->id();
}
