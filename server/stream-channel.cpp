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
#include <config.h>

#include <common/generated_server_marshallers.h>
#include <common/recorder.h>
#include <spice/stream-device.h>

#include "red-channel-client.h"
#include "stream-channel.h"
#include "reds.h"
#include "common-graphics-channel.h"
#include "display-limits.h"
#include "video-stream.h" // TODO remove, put common stuff

/* we need to inherit from CommonGraphicsChannelClient
 * to get buffer handling */
class StreamChannelClient final: public CommonGraphicsChannelClient
{
protected:
    ~StreamChannelClient() override;
public:
    using CommonGraphicsChannelClient::CommonGraphicsChannelClient;

    /* current video stream id, <0 if not initialized or
     * we are not sending a stream */
    int stream_id = -1;
private:
    StreamChannel* get_channel()
    {
        return static_cast<StreamChannel*>(CommonGraphicsChannelClient::get_channel());
    }
    /* Array with SPICE_VIDEO_CODEC_TYPE_ENUM_END elements, with the client
     * preference order (index) as value */
    GArray *client_preferred_video_codecs;
    bool handle_preferred_video_codec_type(SpiceMsgcDisplayPreferredVideoCodecType *msg);
    void marshall_monitors_config(StreamChannel *channel, SpiceMarshaller *m);
    void fill_base(SpiceMarshaller *m, const StreamChannel *channel);
    void on_disconnect() override;
    bool handle_message(uint16_t type, uint32_t size, void *msg) override;
    void send_item(RedPipeItem *pipe_item) override;
};

enum {
    RED_PIPE_ITEM_TYPE_SURFACE_CREATE = RED_PIPE_ITEM_TYPE_COMMON_LAST,
    RED_PIPE_ITEM_TYPE_SURFACE_DESTROY,
    RED_PIPE_ITEM_TYPE_FILL_SURFACE,
    RED_PIPE_ITEM_TYPE_STREAM_CREATE,
    RED_PIPE_ITEM_TYPE_STREAM_DATA,
    RED_PIPE_ITEM_TYPE_STREAM_DESTROY,
    RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT,
    RED_PIPE_ITEM_TYPE_MONITORS_CONFIG,
};

struct StreamCreateItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_STREAM_CREATE> {
    SpiceMsgDisplayStreamCreate stream_create;
};

struct StreamDataItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_STREAM_DATA> {
    ~StreamDataItem() override;

    StreamChannel *channel;
    // NOTE: this must be the last field in the structure
    SpiceMsgDisplayStreamData data;
};

#define PRIMARY_SURFACE_ID 0

RECORDER(stream_channel_data, 32, "Stream channel data packet");

StreamChannelClient::~StreamChannelClient()
{
    g_clear_pointer(&client_preferred_video_codecs, g_array_unref);
}

void StreamChannel::request_new_stream(StreamMsgStartStop *start)
{
    if (start_cb) {
        start_cb(start_opaque, start, this);
    }
}

void
StreamChannelClient::on_disconnect()
{
    StreamChannel *channel = get_channel();

    // if there are still some client connected keep streaming
    // TODO, maybe would be worth sending new codecs if they are better
    if (channel->is_connected()) {
        return;
    }

    channel->stream_id = -1;
    channel->width = 0;
    channel->height = 0;

    // send stream stop to device
    StreamMsgStartStop stop = { 0, };
    get_channel()->request_new_stream(&stop);
}

static StreamChannelClient*
stream_channel_client_new(StreamChannel *channel, RedClient *client, RedStream *stream,
                          int mig_target, RedChannelCapabilities *caps)
{
    auto rcc =
        red::make_shared<StreamChannelClient>(channel, client, stream, caps);
    if (!rcc->init()) {
        return nullptr;
    }
    return rcc.get();
}

void
StreamChannelClient::fill_base(SpiceMarshaller *m, const StreamChannel *channel)
{
    SpiceMsgDisplayBase base;

    base.surface_id = PRIMARY_SURFACE_ID;
    base.box = (SpiceRect) { 0, 0, channel->width, channel->height };
    base.clip = (SpiceClip) { SPICE_CLIP_TYPE_NONE, nullptr };

    spice_marshall_DisplayBase(m, &base);
}

void
StreamChannelClient::marshall_monitors_config(StreamChannel *channel, SpiceMarshaller *m)
{
    struct {
        SpiceMsgDisplayMonitorsConfig config;
        SpiceHead head;
    } msg = {
        { 1, 1, },
        {
            // monitor ID. These IDs are allocated per channel starting from 0
            0,
            PRIMARY_SURFACE_ID,
            channel->width, channel->height,
            0, 0,
            0 // flags
        }
    };

    init_send_data(SPICE_MSG_DISPLAY_MONITORS_CONFIG);
    spice_marshall_msg_display_monitors_config(m, &msg.config);
}

void StreamChannelClient::send_item(RedPipeItem *pipe_item)
{
    SpiceMarshaller *m = get_marshaller();
    StreamChannel *channel = get_channel();

    switch (pipe_item->type) {
    case RED_PIPE_ITEM_TYPE_SURFACE_CREATE: {
        init_send_data(SPICE_MSG_DISPLAY_SURFACE_CREATE);
        SpiceMsgSurfaceCreate surface_create = {
            PRIMARY_SURFACE_ID,
            channel->width, channel->height,
            SPICE_SURFACE_FMT_32_xRGB, SPICE_SURFACE_FLAGS_PRIMARY
        };

        // give an hint to client that we are sending just streaming
        // see spice.proto for capability check here
        if (test_remote_cap(SPICE_DISPLAY_CAP_MULTI_CODEC)) {
            surface_create.flags |= SPICE_SURFACE_FLAGS_STREAMING_MODE;
        }

        spice_marshall_msg_display_surface_create(m, &surface_create);
        break;
    }
    case RED_PIPE_ITEM_TYPE_MONITORS_CONFIG:
        if (!test_remote_cap(SPICE_DISPLAY_CAP_MONITORS_CONFIG)) {
            return;
        }
        marshall_monitors_config(channel, m);
        break;
    case RED_PIPE_ITEM_TYPE_SURFACE_DESTROY: {
        init_send_data(SPICE_MSG_DISPLAY_SURFACE_DESTROY);
        SpiceMsgSurfaceDestroy surface_destroy = { PRIMARY_SURFACE_ID };
        spice_marshall_msg_display_surface_destroy(m, &surface_destroy);
        break;
    }
    case RED_PIPE_ITEM_TYPE_FILL_SURFACE: {
        init_send_data(SPICE_MSG_DISPLAY_DRAW_FILL);

        fill_base(m, channel);

        SpiceFill fill;
        fill.brush = (SpiceBrush) { SPICE_BRUSH_TYPE_SOLID, { .color = 0 } };
        fill.rop_descriptor = SPICE_ROPD_OP_PUT;
        fill.mask = (SpiceQMask) { 0, { 0, 0 }, nullptr };
        SpiceMarshaller *brush_pat_out, *mask_bitmap_out;
        spice_marshall_Fill(m, &fill, &brush_pat_out, &mask_bitmap_out);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_CREATE: {
        auto item = static_cast<StreamCreateItem*>(pipe_item);
        stream_id = item->stream_create.id;
        init_send_data(SPICE_MSG_DISPLAY_STREAM_CREATE);
        spice_marshall_msg_display_stream_create(m, &item->stream_create);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT: {
        if (stream_id < 0
            || !test_remote_cap(SPICE_DISPLAY_CAP_STREAM_REPORT)) {
            return;
        }
        SpiceMsgDisplayStreamActivateReport msg;
        msg.stream_id = stream_id;
        msg.unique_id = 1; // TODO useful ?
        msg.max_window_size = RED_STREAM_CLIENT_REPORT_WINDOW;
        msg.timeout_ms = RED_STREAM_CLIENT_REPORT_TIMEOUT;
        init_send_data(SPICE_MSG_DISPLAY_STREAM_ACTIVATE_REPORT);
        spice_marshall_msg_display_stream_activate_report(m, &msg);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_DATA: {
        auto item = static_cast<StreamDataItem*>(pipe_item);
        init_send_data(SPICE_MSG_DISPLAY_STREAM_DATA);
        spice_marshall_msg_display_stream_data(m, &item->data);
        pipe_item->add_to_marshaller(m, item->data.data, item->data.data_size);
        record(stream_channel_data, "Stream data packet size %u mm_time %u",
               item->data.data_size, item->data.base.multi_media_time);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_DESTROY: {
        if (stream_id < 0) {
            return;
        }
        SpiceMsgDisplayStreamDestroy stream_destroy = { stream_id };
        init_send_data(SPICE_MSG_DISPLAY_STREAM_DESTROY);
        spice_marshall_msg_display_stream_destroy(m, &stream_destroy);
        stream_id = -1;
        break;
    }
    default:
        spice_error("invalid pipe item type");
    }

    begin_send_message();
}

bool StreamChannelClient::handle_message(uint16_t type, uint32_t size, void *msg)
{
    switch (type) {
    case SPICE_MSGC_DISPLAY_INIT:
    case SPICE_MSGC_DISPLAY_PREFERRED_COMPRESSION:
        return true;
    case SPICE_MSGC_DISPLAY_STREAM_REPORT:
        /* TODO these will help tune the streaming reducing/increasing quality */
        return true;
    case SPICE_MSGC_DISPLAY_GL_DRAW_DONE:
        /* client should not send this message */
        return false;
    case SPICE_MSGC_DISPLAY_PREFERRED_VIDEO_CODEC_TYPE:
        return handle_preferred_video_codec_type(
            (SpiceMsgcDisplayPreferredVideoCodecType *)msg);
    default:
        return CommonGraphicsChannelClient::handle_message(type, size, msg);
    }
}


red::shared_ptr<StreamChannel>
stream_channel_new(RedsState *server, uint32_t id)
{
    // TODO this id should be after all qxl devices
    return red::make_shared<StreamChannel>(server, id);
}

#define MAX_SUPPORTED_CODECS SPICE_VIDEO_CODEC_TYPE_ENUM_END

// find common codecs supported by all clients
static uint8_t
stream_channel_get_supported_codecs(StreamChannel *channel, uint8_t *out_codecs)
{
    RedChannelClient *rcc;
    int codec;

    static const uint16_t codec2cap[] = {
        0, // invalid
        SPICE_DISPLAY_CAP_CODEC_MJPEG,
        SPICE_DISPLAY_CAP_CODEC_VP8,
        SPICE_DISPLAY_CAP_CODEC_H264,
        SPICE_DISPLAY_CAP_CODEC_VP9,
        SPICE_DISPLAY_CAP_CODEC_H265,
    };

    bool supported[SPICE_N_ELEMENTS(codec2cap)];

    for (codec = 0; codec < SPICE_N_ELEMENTS(codec2cap); ++codec) {
        supported[codec] = true;
    }

    FOREACH_CLIENT(channel, rcc) {
        for (codec = 1; codec < SPICE_N_ELEMENTS(codec2cap); ++codec) {
            // if do not support codec delete from list
            if (!rcc->test_remote_cap(codec2cap[codec])) {
                supported[codec] = false;
            }
        }
    }

    // surely mjpeg is supported
    supported[SPICE_VIDEO_CODEC_TYPE_MJPEG] = true;

    int num = 0;
    for (codec = 1; codec < SPICE_N_ELEMENTS(codec2cap); ++codec) {
        if (supported[codec]) {
            out_codecs[num++] = codec;
        }
    }

    return num;
}

bool
StreamChannelClient::handle_preferred_video_codec_type(SpiceMsgcDisplayPreferredVideoCodecType *msg)
{
    if (msg->num_of_codecs == 0) {
        return true;
    }

    g_clear_pointer(&client_preferred_video_codecs, g_array_unref);
    client_preferred_video_codecs = video_stream_parse_preferred_codecs(msg);

    return true;
}

void StreamChannel::on_connect(RedClient *red_client, RedStream *stream,
                               int migration, RedChannelCapabilities *caps)
{
    StreamChannelClient *client;
    struct {
        StreamMsgStartStop base;
        uint8_t codecs_buffer[MAX_SUPPORTED_CODECS];
    } start_msg;
    StreamMsgStartStop *const start = &start_msg.base;

    spice_return_if_fail(stream != nullptr);

    client = stream_channel_client_new(this, red_client, stream, migration, caps);
    if (client == nullptr) {
        return;
    }

    // request new stream
    start->num_codecs = stream_channel_get_supported_codecs(this, start->codecs);
    // send in any case, even if list is not changed
    // notify device about changes
    request_new_stream(start);


    // see guest_set_client_capabilities
    RedChannelClient *rcc = client;
    rcc->push_set_ack();

    // TODO what should happen on migration, dcc return if on migration wait ??
    rcc->ack_zero_messages_window();

    // "emulate" dcc_start
    rcc->pipe_add_empty_msg(SPICE_MSG_DISPLAY_INVAL_ALL_PALETTES);

    // only if "surface"
    if (width == 0 || height == 0) {
        return;
    }

    // pass proper data
    rcc->pipe_add_type(RED_PIPE_ITEM_TYPE_SURFACE_CREATE);
    rcc->pipe_add_type(RED_PIPE_ITEM_TYPE_MONITORS_CONFIG);
    // surface data
    rcc->pipe_add_type(RED_PIPE_ITEM_TYPE_FILL_SURFACE);
    // TODO monitor configs ??
    rcc->pipe_add_empty_msg(SPICE_MSG_DISPLAY_MARK);
}

StreamChannel::StreamChannel(RedsState *reds, uint32_t id):
    RedChannel(reds, SPICE_CHANNEL_DISPLAY, id, RedChannel::HandleAcks)
{
    set_cap(SPICE_DISPLAY_CAP_MONITORS_CONFIG);
    set_cap(SPICE_DISPLAY_CAP_STREAM_REPORT);
    set_cap(SPICE_DISPLAY_CAP_PREF_VIDEO_CODEC_TYPE);

    reds_register_channel(reds, this);
}

void
StreamChannel::change_format(const StreamMsgFormat *fmt)
{
    // send destroy old stream
    pipes_add_type(RED_PIPE_ITEM_TYPE_STREAM_DESTROY);

    // send new create surface if required
    if (width != fmt->width || height != fmt->height) {
        if (width != 0 && height != 0) {
            pipes_add_type(RED_PIPE_ITEM_TYPE_SURFACE_DESTROY);
        }
        width = fmt->width;
        height = fmt->height;
        pipes_add_type(RED_PIPE_ITEM_TYPE_SURFACE_CREATE);
        pipes_add_type(RED_PIPE_ITEM_TYPE_MONITORS_CONFIG);
        // TODO monitors config ??
        pipes_add_empty_msg(SPICE_MSG_DISPLAY_MARK);
    }

    // allocate a new stream id
    stream_id = (stream_id + 1) % NUM_STREAMS;

    // send create stream
    auto item = red::make_shared<StreamCreateItem>();
    item->stream_create.id = stream_id;
    item->stream_create.flags = SPICE_STREAM_FLAGS_TOP_DOWN;
    item->stream_create.codec_type = fmt->codec;
    item->stream_create.stream_width = fmt->width;
    item->stream_create.stream_height = fmt->height;
    item->stream_create.src_width = fmt->width;
    item->stream_create.src_height = fmt->height;
    item->stream_create.dest = (SpiceRect) { 0, 0, fmt->width, fmt->height };
    item->stream_create.clip = (SpiceClip) { SPICE_CLIP_TYPE_NONE, nullptr };
    pipes_add(item);

    // activate stream report if possible
    pipes_add_type(RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT);
}

inline void
StreamChannel::update_queue_stat(int32_t num_diff, int32_t size_diff)
{
    queue_stat.num_items += num_diff;
    queue_stat.size += size_diff;
    if (queue_cb) {
        queue_cb(queue_opaque, &queue_stat, this);
    }
}

StreamDataItem::~StreamDataItem()
{
    channel->update_queue_stat(-1, -data.data_size);
}

void
StreamChannel::send_data(const void *data, size_t size, uint32_t mm_time)
{
    if (stream_id < 0) {
        // this condition can happen if the guest didn't handle
        // the format stop that we send so think the stream is still
        // started
        return;
    }

    auto item = new (size) StreamDataItem();
    item->data.base.id = stream_id;
    item->data.base.multi_media_time = mm_time;
    item->data.data_size = size;
    item->channel = this;
    update_queue_stat(1, size);
    // TODO try to optimize avoiding the copy
    memcpy(item->data.data, data, size);
    pipes_add(red::shared_ptr<StreamDataItem>(item));
}

void
StreamChannel::register_start_cb(stream_channel_start_proc cb, void *opaque)
{
    start_cb = cb;
    start_opaque = opaque;
}

void
StreamChannel::register_queue_stat_cb(stream_channel_queue_stat_proc cb, void *opaque)
{
    queue_cb = cb;
    queue_opaque = opaque;
}

void
StreamChannel::reset()
{
    struct {
        StreamMsgStartStop base;
        uint8_t codecs_buffer[MAX_SUPPORTED_CODECS];
    } start_msg;
    StreamMsgStartStop *const start = &start_msg.base;

    // send destroy old stream
    pipes_add_type(RED_PIPE_ITEM_TYPE_STREAM_DESTROY);

    // destroy display surface
    if (width != 0 && height != 0) {
        pipes_add_type(RED_PIPE_ITEM_TYPE_SURFACE_DESTROY);
    }

    stream_id = -1;
    width = 0;
    height = 0;

    if (!is_connected()) {
        return;
    }

    // try to request a new stream, this should start a new stream
    // if the guest is connected to the device and a client is already connected
    start->num_codecs = stream_channel_get_supported_codecs(this, start->codecs);
    // send in any case, even if list is not changed
    // notify device about changes
    request_new_stream(start);
}
