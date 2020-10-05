/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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
*/
#include <config.h>

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#endif

#include <common/generated_server_marshallers.h>
#include <common/snd_codec.h>

#include "spice-wrapped.h"
#include "red-common.h"
#include "main-channel.h"
#include "reds.h"
#include "red-channel-client.h"
#include "red-client.h"
#include "sound.h"
#include "main-channel-client.h"

#define SND_RECEIVE_BUF_SIZE     (16 * 1024 * 2)
#define RECORD_SAMPLES_SIZE (SND_RECEIVE_BUF_SIZE >> 2)

enum SndCommand {
    SND_MIGRATE,
    SND_CTRL,
    SND_VOLUME,
    SND_MUTE,
    SND_END_COMMAND,
};

enum PlaybackCommand {
    SND_PLAYBACK_MODE = SND_END_COMMAND,
    SND_PLAYBACK_PCM,
    SND_PLAYBACK_LATENCY,
};

#define SND_MIGRATE_MASK (1 << SND_MIGRATE)
#define SND_CTRL_MASK (1 << SND_CTRL)
#define SND_VOLUME_MASK (1 << SND_VOLUME)
#define SND_MUTE_MASK (1 << SND_MUTE)
#define SND_VOLUME_MUTE_MASK (SND_VOLUME_MASK|SND_MUTE_MASK)

#define SND_PLAYBACK_MODE_MASK (1 << SND_PLAYBACK_MODE)
#define SND_PLAYBACK_PCM_MASK (1 << SND_PLAYBACK_PCM)
#define SND_PLAYBACK_LATENCY_MASK ( 1 << SND_PLAYBACK_LATENCY)

struct SndChannelClient;
struct SndChannel;
struct PlaybackChannelClient;
struct RecordChannelClient;
struct AudioFrame;
struct AudioFrameContainer;

enum {
    RED_PIPE_ITEM_PERSISTENT = RED_PIPE_ITEM_TYPE_CHANNEL_BASE,
};

/* This pipe item is never deleted and added to the queue when messages
 * have to be sent.
 * This is used to have a simple item in RedChannelClient queue but to send
 * multiple messages in a row if possible.
 * During realtime sound transmission you usually don't want to queue too
 * much data or having retransmission preferring instead loosing some
 * samples.
 */
struct PersistentPipeItem final: public RedPipeItemNum<RED_PIPE_ITEM_PERSISTENT>
{
    PersistentPipeItem();
};

/* Connects an audio client to a Spice client */
class SndChannelClient: public RedChannelClient
{
public:
    using RedChannelClient::RedChannelClient;

    bool active;
    bool client_active;

    uint32_t command;

    PersistentPipeItem persistent_pipe_item;

    inline SndChannel* get_channel();

    bool config_socket() override;
    uint8_t *alloc_recv_buf(uint16_t type, uint32_t size) override;
    void release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg) override;
    void migrate() override;

private:
    /* we don't expect very big messages so don't allocate too much
     * bytes, data will be cached in RecordChannelClient::samples */
    uint8_t receive_buf[SND_CODEC_MAX_FRAME_BYTES + 64];
};

static void snd_playback_alloc_frames(PlaybackChannelClient *playback);


struct AudioFrame {
    uint32_t time;
    uint32_t samples[SND_CODEC_MAX_FRAME_SIZE];
    PlaybackChannelClient *client;
    AudioFrame *next;
    AudioFrameContainer *container;
    bool allocated;
};

#define NUM_AUDIO_FRAMES 3
struct AudioFrameContainer
{
    int refs;
    AudioFrame items[NUM_AUDIO_FRAMES];
};

class PlaybackChannelClient final: public SndChannelClient
{
protected:
    ~PlaybackChannelClient() override;
public:
    PlaybackChannelClient(PlaybackChannel *channel,
                          RedClient *client,
                          RedStream *stream,
                          RedChannelCapabilities *caps);
    bool init() override;

    AudioFrameContainer *frames = nullptr;
    AudioFrame *free_frames = nullptr;
    AudioFrame *in_progress = nullptr;   /* Frame being sent to the client */
    AudioFrame *pending_frame = nullptr; /* Next frame to send to the client */
    uint32_t mode = SPICE_AUDIO_DATA_MODE_RAW;
    uint32_t latency = 0;
    SndCodec codec = nullptr;
    uint8_t  encode_buf[SND_CODEC_MAX_COMPRESSED_BYTES];

    static void on_message_marshalled(uint8_t *data, void *opaque);
protected:
    void send_item(RedPipeItem *item) override;
};

struct SpiceVolumeState {
    uint16_t *volume;
    uint8_t volume_nchannels;
    int mute;
};

/* Base class for PlaybackChannel and RecordChannel */
struct SndChannel: public RedChannel
{
    using RedChannel::RedChannel;
    ~SndChannel() override;
    void set_peer_common();
    bool active;
    SpiceVolumeState volume;
    uint32_t frequency = SND_CODEC_OPUS_PLAYBACK_FREQ;
};

inline SndChannel* SndChannelClient::get_channel()
{
    return static_cast<SndChannel*>(RedChannelClient::get_channel());
}

struct PlaybackChannel final: public SndChannel
{
    explicit PlaybackChannel(RedsState *reds);
    void on_connect(RedClient *client, RedStream *stream,
                    int migration, RedChannelCapabilities *caps) override;
};


struct RecordChannel final: public SndChannel
{
    explicit RecordChannel(RedsState *reds);
    void on_connect(RedClient *client, RedStream *stream,
                    int migration, RedChannelCapabilities *caps) override;
};


class RecordChannelClient final: public SndChannelClient
{
protected:
    ~RecordChannelClient() override;
public:
    using SndChannelClient::SndChannelClient;
    bool init() override;

    uint32_t samples[RECORD_SAMPLES_SIZE];
    uint32_t write_pos = 0;
    uint32_t read_pos = 0;
    uint32_t mode = SPICE_AUDIO_DATA_MODE_RAW;
    uint32_t mode_time = 0;
    uint32_t start_time = 0;
    SndCodec codec = nullptr;
    uint8_t  decode_buf[SND_CODEC_MAX_FRAME_BYTES];
protected:
    bool handle_message(uint16_t type, uint32_t size, void *message) override;
    void send_item(RedPipeItem *item) override;
};


/* A list of all Spice{Playback,Record}State objects */
static GList *snd_channels;

static void snd_send(SndChannelClient * client);

/* sound channels only support a single client */
static SndChannelClient *snd_channel_get_client(SndChannel *channel)
{
    GList *clients = channel->get_clients();
    if (clients == nullptr) {
        return nullptr;
    }

    return (SndChannelClient*) clients->data;
}

static RedsState* snd_channel_get_server(SndChannelClient *client)
{
    g_return_val_if_fail(client != nullptr, NULL);
    return client->get_channel()->get_server();
}

static void snd_playback_free_frame(PlaybackChannelClient *playback_client, AudioFrame *frame)
{
    frame->client = playback_client;
    frame->next = playback_client->free_frames;
    playback_client->free_frames = frame;
}

void PlaybackChannelClient::on_message_marshalled(uint8_t *, void *opaque)
{
    auto client = reinterpret_cast<PlaybackChannelClient*>(opaque);

    if (client->in_progress) {
        snd_playback_free_frame(client, client->in_progress);
        client->in_progress = nullptr;
        if (client->pending_frame) {
            client->command |= SND_PLAYBACK_PCM_MASK;
            snd_send(client);
        }
    }
}

static bool snd_record_handle_write(RecordChannelClient *record_client, size_t size, void *message)
{
    SpiceMsgcRecordPacket *packet;
    uint32_t write_pos;
    uint8_t* data;
    uint32_t len;
    uint32_t now;

    if (!record_client) {
        return false;
    }

    packet = (SpiceMsgcRecordPacket *)message;

    if (record_client->mode == SPICE_AUDIO_DATA_MODE_RAW) {
        data = packet->data;
        size = packet->data_size >> 2;
        size = MIN(size, RECORD_SAMPLES_SIZE);
     } else {
        int decode_size;
        decode_size = sizeof(record_client->decode_buf);
        if (snd_codec_decode(record_client->codec, packet->data, packet->data_size,
                    record_client->decode_buf, &decode_size) != SND_CODEC_OK)
            return false;
        data = record_client->decode_buf;
        size = decode_size >> 2;
    }

    write_pos = record_client->write_pos % RECORD_SAMPLES_SIZE;
    record_client->write_pos += size;
    len = RECORD_SAMPLES_SIZE - write_pos;
    now = MIN(len, size);
    size -= now;
    memcpy(record_client->samples + write_pos, data, now << 2);

    if (size) {
        memcpy(record_client->samples, data + now, size << 2);
    }

    if (record_client->write_pos - record_client->read_pos > RECORD_SAMPLES_SIZE) {
        record_client->read_pos = record_client->write_pos - RECORD_SAMPLES_SIZE;
    }
    return true;
}

static
const char* spice_audio_data_mode_to_string(gint mode)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    static const char * const str[] = {
        [ SPICE_AUDIO_DATA_MODE_INVALID ] = "invalid",
        [ SPICE_AUDIO_DATA_MODE_RAW ] = "raw",
        [ SPICE_AUDIO_DATA_MODE_CELT_0_5_1 ] = "celt",
        [ SPICE_AUDIO_DATA_MODE_OPUS ] = "opus",
    };
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (mode >= 0 && mode < G_N_ELEMENTS(str)) {
        return str[mode];
    }

    return "unknown audio codec";
}

XXX_CAST(RedChannelClient, RecordChannelClient, RECORD_CHANNEL_CLIENT)

bool RecordChannelClient::handle_message(uint16_t type, uint32_t size, void *message)
{
    switch (type) {
    case SPICE_MSGC_RECORD_DATA:
        return snd_record_handle_write(this, size, message);
    case SPICE_MSGC_RECORD_MODE: {
        auto msg_mode = (SpiceMsgcRecordMode *)message;
        SndChannel *channel = get_channel();
        mode_time = msg_mode->time;
        if (msg_mode->mode != SPICE_AUDIO_DATA_MODE_RAW) {
            if (snd_codec_is_capable((SpiceAudioDataMode) msg_mode->mode, channel->frequency)) {
                if (snd_codec_create(&codec, (SpiceAudioDataMode) msg_mode->mode,
                                     channel->frequency, SND_CODEC_DECODE) == SND_CODEC_OK) {
                    mode = msg_mode->mode;
                } else {
                    red_channel_warning(channel, "create decoder failed");
                    return false;
                }
            }
            else {
                red_channel_warning(channel, "unsupported mode %d", mode);
                return false;
            }
        }
        else
            mode = msg_mode->mode;

        spice_debug("record client %p using mode %s", this,
                    spice_audio_data_mode_to_string(mode));
        break;
    }

    case SPICE_MSGC_RECORD_START_MARK: {
        auto mark = (SpiceMsgcRecordStartMark *)message;
        start_time = mark->time;
        break;
    }
    default:
        return RedChannelClient::handle_message(type, size, message);
    }
    return true;
}

static bool snd_channel_send_migrate(SndChannelClient *client)
{
    RedChannelClient *rcc = client;
    SpiceMarshaller *m = rcc->get_marshaller();
    SpiceMsgMigrate migrate;

    rcc->init_send_data(SPICE_MSG_MIGRATE);
    migrate.flags = 0;
    spice_marshall_msg_migrate(m, &migrate);

    rcc->begin_send_message();
    return true;
}

static bool snd_playback_send_migrate(PlaybackChannelClient *client)
{
    return snd_channel_send_migrate(client);
}

static bool snd_send_volume(SndChannelClient *client, uint32_t cap, int msg)
{
    SpiceMsgAudioVolume *vol;
    uint8_t c;
    RedChannelClient *rcc = client;
    SpiceMarshaller *m = rcc->get_marshaller();
    SndChannel *channel = client->get_channel();
    SpiceVolumeState *st = &channel->volume;

    if (!rcc->test_remote_cap(cap)) {
        return false;
    }

    vol = (SpiceMsgAudioVolume*) alloca(sizeof (SpiceMsgAudioVolume) +
                 st->volume_nchannels * sizeof (uint16_t));
    rcc->init_send_data(msg);
    vol->nchannels = st->volume_nchannels;
    for (c = 0; c < st->volume_nchannels; ++c) {
        vol->volume[c] = st->volume[c];
    }
    spice_marshall_SpiceMsgAudioVolume(m, vol);

    rcc->begin_send_message();
    return true;
}

static bool snd_playback_send_volume(PlaybackChannelClient *playback_client)
{
    return snd_send_volume(playback_client, SPICE_PLAYBACK_CAP_VOLUME,
                           SPICE_MSG_PLAYBACK_VOLUME);
}

static bool snd_send_mute(SndChannelClient *client, uint32_t cap, int msg)
{
    SpiceMsgAudioMute mute;
    RedChannelClient *rcc = client;
    SpiceMarshaller *m = rcc->get_marshaller();
    SndChannel *channel = client->get_channel();
    SpiceVolumeState *st = &channel->volume;

    if (!rcc->test_remote_cap(cap)) {
        return false;
    }

    rcc->init_send_data(msg);
    mute.mute = st->mute;
    spice_marshall_SpiceMsgAudioMute(m, &mute);

    rcc->begin_send_message();
    return true;
}

static bool snd_playback_send_mute(PlaybackChannelClient *playback_client)
{
    return snd_send_mute(playback_client, SPICE_PLAYBACK_CAP_VOLUME,
                         SPICE_MSG_PLAYBACK_MUTE);
}

static bool snd_playback_send_latency(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = playback_client;
    SpiceMarshaller *m = rcc->get_marshaller();
    SpiceMsgPlaybackLatency latency_msg;

    spice_debug("latency %u", playback_client->latency);
    rcc->init_send_data(SPICE_MSG_PLAYBACK_LATENCY);
    latency_msg.latency_ms = playback_client->latency;
    spice_marshall_msg_playback_latency(m, &latency_msg);

    rcc->begin_send_message();
    return true;
}

static bool snd_playback_send_start(PlaybackChannelClient *playback_client)
{
    SpiceMarshaller *m = playback_client->get_marshaller();
    SpiceMsgPlaybackStart start;

    playback_client->init_send_data(SPICE_MSG_PLAYBACK_START);
    start.channels = SPICE_INTERFACE_PLAYBACK_CHAN;
    start.frequency = playback_client->get_channel()->frequency;
    spice_assert(SPICE_INTERFACE_PLAYBACK_FMT == SPICE_INTERFACE_AUDIO_FMT_S16);
    start.format = SPICE_AUDIO_FMT_S16;
    start.time = reds_get_mm_time();
    spice_marshall_msg_playback_start(m, &start);

    playback_client->begin_send_message();
    return true;
}

static bool snd_playback_send_stop(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = playback_client;

    rcc->init_send_data(SPICE_MSG_PLAYBACK_STOP);

    rcc->begin_send_message();
    return true;
}

static int snd_playback_send_ctl(PlaybackChannelClient *playback_client)
{
    SndChannelClient *client = playback_client;

    if ((client->client_active = client->active)) {
        return snd_playback_send_start(playback_client);
    }

    return snd_playback_send_stop(playback_client);
}

static bool snd_record_send_start(RecordChannelClient *record_client)
{
    RedChannelClient *rcc = record_client;
    SpiceMarshaller *m = rcc->get_marshaller();
    SpiceMsgRecordStart start;

    rcc->init_send_data(SPICE_MSG_RECORD_START);

    start.channels = SPICE_INTERFACE_RECORD_CHAN;
    start.frequency = record_client->get_channel()->frequency;
    spice_assert(SPICE_INTERFACE_RECORD_FMT == SPICE_INTERFACE_AUDIO_FMT_S16);
    start.format = SPICE_AUDIO_FMT_S16;
    spice_marshall_msg_record_start(m, &start);

    rcc->begin_send_message();
    return true;
}

static bool snd_record_send_stop(RecordChannelClient *record_client)
{
    RedChannelClient *rcc = record_client;

    rcc->init_send_data(SPICE_MSG_RECORD_STOP);

    rcc->begin_send_message();
    return true;
}

static int snd_record_send_ctl(RecordChannelClient *record_client)
{
    SndChannelClient *client = record_client;

    if ((client->client_active = client->active)) {
        return snd_record_send_start(record_client);
    }

    return snd_record_send_stop(record_client);
}

static bool snd_record_send_volume(RecordChannelClient *record_client)
{
    return snd_send_volume(record_client, SPICE_RECORD_CAP_VOLUME,
                           SPICE_MSG_RECORD_VOLUME);
}

static bool snd_record_send_mute(RecordChannelClient *record_client)
{
    return snd_send_mute(record_client, SPICE_RECORD_CAP_VOLUME,
                         SPICE_MSG_RECORD_MUTE);
}

static bool snd_record_send_migrate(RecordChannelClient *record_client)
{
    /* No need for migration data: if recording has started before migration,
     * the client receives RECORD_STOP from the src before the migration completion
     * notification (when the vm is stopped).
     * Afterwards, when the vm starts on the dest, the client receives RECORD_START. */
    return snd_channel_send_migrate(record_client);
}

static bool snd_playback_send_write(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = playback_client;
    SpiceMarshaller *m = rcc->get_marshaller();
    AudioFrame *frame;
    SpiceMsgPlaybackPacket msg;

    rcc->init_send_data(SPICE_MSG_PLAYBACK_DATA);

    frame = playback_client->in_progress;
    msg.time = frame->time;

    spice_marshall_msg_playback_data(m, &msg);

    if (playback_client->mode == SPICE_AUDIO_DATA_MODE_RAW) {
        spice_marshaller_add_by_ref_full(m, (uint8_t *)frame->samples,
                                         snd_codec_frame_size(playback_client->codec) *
                                         sizeof(frame->samples[0]),
                                         PlaybackChannelClient::on_message_marshalled,
                                         playback_client);
    }
    else {
        int n = sizeof(playback_client->encode_buf);
        if (snd_codec_encode(playback_client->codec, (uint8_t *) frame->samples,
                                    snd_codec_frame_size(playback_client->codec) * sizeof(frame->samples[0]),
                                    playback_client->encode_buf, &n) != SND_CODEC_OK) {
            red_channel_warning(rcc->get_channel(), "encode failed");
            rcc->disconnect();
            return false;
        }
        spice_marshaller_add_by_ref_full(m, playback_client->encode_buf, n,
                                         PlaybackChannelClient::on_message_marshalled,
                                         playback_client);
    }

    rcc->begin_send_message();
    return true;
}

static bool playback_send_mode(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = playback_client;
    SpiceMarshaller *m = rcc->get_marshaller();
    SpiceMsgPlaybackMode mode;

    rcc->init_send_data(SPICE_MSG_PLAYBACK_MODE);
    mode.time = reds_get_mm_time();
    mode.mode = playback_client->mode;
    spice_marshall_msg_playback_mode(m, &mode);

    rcc->begin_send_message();
    return true;
}

PersistentPipeItem::PersistentPipeItem()
{
    // force this item to stay alive
    shared_ptr_add_ref(this);
}

static void snd_send(SndChannelClient * client)
{
    if (!client->pipe_is_empty()|| !client->command) {
        return;
    }
    // just append a dummy item and push!
    RedPipeItemPtr item(&client->persistent_pipe_item);
    client->pipe_add_push(std::move(item));
}

XXX_CAST(RedChannelClient, PlaybackChannelClient, PLAYBACK_CHANNEL_CLIENT)

void PlaybackChannelClient::send_item(G_GNUC_UNUSED RedPipeItem *item)
{
    command &= SND_PLAYBACK_MODE_MASK|SND_PLAYBACK_PCM_MASK|
               SND_CTRL_MASK|SND_VOLUME_MUTE_MASK|
               SND_MIGRATE_MASK|SND_PLAYBACK_LATENCY_MASK;
    while (command) {
        if (command & SND_PLAYBACK_MODE_MASK) {
            command &= ~SND_PLAYBACK_MODE_MASK;
            if (playback_send_mode(this)) {
                break;
            }
        }
        if (command & SND_PLAYBACK_PCM_MASK) {
            spice_assert(!in_progress && pending_frame);
            in_progress = pending_frame;
            pending_frame = nullptr;
            command &= ~SND_PLAYBACK_PCM_MASK;
            if (snd_playback_send_write(this)) {
                break;
            }
            red_channel_warning(get_channel(),
                                "snd_send_playback_write failed");
        }
        if (command & SND_CTRL_MASK) {
            command &= ~SND_CTRL_MASK;
            if (snd_playback_send_ctl(this)) {
                break;
            }
        }
        if (command & SND_VOLUME_MASK) {
            command &= ~SND_VOLUME_MASK;
            if (snd_playback_send_volume(this)) {
                break;
            }
        }
        if (command & SND_MUTE_MASK) {
            command &= ~SND_MUTE_MASK;
            if (snd_playback_send_mute(this)) {
                break;
            }
        }
        if (command & SND_MIGRATE_MASK) {
            command &= ~SND_MIGRATE_MASK;
            if (snd_playback_send_migrate(this)) {
                break;
            }
        }
        if (command & SND_PLAYBACK_LATENCY_MASK) {
            command &= ~SND_PLAYBACK_LATENCY_MASK;
            if (snd_playback_send_latency(this)) {
                break;
            }
        }
    }
    snd_send(this);
}

void RecordChannelClient::send_item(G_GNUC_UNUSED RedPipeItem *item)
{
    command &= SND_CTRL_MASK|SND_VOLUME_MUTE_MASK|SND_MIGRATE_MASK;
    while (command) {
        if (command & SND_CTRL_MASK) {
            command &= ~SND_CTRL_MASK;
            if (snd_record_send_ctl(this)) {
                break;
            }
        }
        if (command & SND_VOLUME_MASK) {
            command &= ~SND_VOLUME_MASK;
            if (snd_record_send_volume(this)) {
                break;
            }
        }
        if (command & SND_MUTE_MASK) {
            command &= ~SND_MUTE_MASK;
            if (snd_record_send_mute(this)) {
                break;
            }
        }
        if (command & SND_MIGRATE_MASK) {
            command &= ~SND_MIGRATE_MASK;
            if (snd_record_send_migrate(this)) {
                break;
            }
        }
    }
    snd_send(this);
}

bool SndChannelClient::config_socket()
{
    RedStream *stream = get_stream();
    RedClient *red_client = get_client();
    MainChannelClient *mcc = red_client->get_main();

#ifdef SO_PRIORITY
    int priority = 6;
    if (setsockopt(stream->socket, SOL_SOCKET, SO_PRIORITY, (void*)&priority,
                   sizeof(priority)) == -1) {
        if (errno != ENOTSUP) {
            red_channel_warning(get_channel(),
                                "setsockopt failed, %s", strerror(errno));
        }
    }
#endif

#ifdef IPTOS_LOWDELAY
    int tos = IPTOS_LOWDELAY;
    if (setsockopt(stream->socket, IPPROTO_IP, IP_TOS, (void*)&tos, sizeof(tos)) == -1) {
        if (errno != ENOTSUP) {
            red_channel_warning(get_channel(),
                                "setsockopt failed, %s",
                                strerror(errno));
        }
    }
#endif

    red_stream_set_no_delay(stream, !mcc->is_low_bandwidth());

    return true;
}

uint8_t*
SndChannelClient::alloc_recv_buf(uint16_t type, uint32_t size)
{
    // If message is too big allocate one, this should never happen
    if (size > sizeof(receive_buf)) {
        return (uint8_t*) g_malloc(size);
    }
    return receive_buf;
}

void
SndChannelClient::release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg)
{
    if (msg != receive_buf) {
        g_free(msg);
    }
}

static void snd_set_command(SndChannelClient *client, uint32_t command)
{
    if (!client) {
        return;
    }
    client->command |= command;
}

static void snd_channel_set_volume(SndChannel *channel,
                                   uint8_t nchannels, uint16_t *volume)
{
    SpiceVolumeState *st = &channel->volume;
    SndChannelClient *client = snd_channel_get_client(channel);

    st->volume_nchannels = nchannels;
    g_free(st->volume);
    st->volume = (uint16_t*) g_memdup(volume, sizeof(uint16_t) * nchannels);

    if (!client || nchannels == 0)
        return;

    snd_set_command(client, SND_VOLUME_MASK);
    snd_send(client);
}

SPICE_GNUC_VISIBLE void spice_server_playback_set_volume(SpicePlaybackInstance *sin,
                                                  uint8_t nchannels,
                                                  uint16_t *volume)
{
    snd_channel_set_volume(sin->st, nchannels, volume);
}

static void snd_channel_set_mute(SndChannel *channel, uint8_t mute)
{
    SpiceVolumeState *st = &channel->volume;
    SndChannelClient *client = snd_channel_get_client(channel);

    st->mute = mute;

    if (!client)
        return;

    snd_set_command(client, SND_MUTE_MASK);
    snd_send(client);
}

SPICE_GNUC_VISIBLE void spice_server_playback_set_mute(SpicePlaybackInstance *sin, uint8_t mute)
{
    snd_channel_set_mute(sin->st, mute);
}

static void snd_channel_client_start(SndChannelClient *client)
{
    spice_assert(!client->active);
    client->active = true;
    if (!client->client_active) {
        snd_set_command(client, SND_CTRL_MASK);
        snd_send(client);
    } else {
        client->command &= ~SND_CTRL_MASK;
    }
}

static void playback_channel_client_start(SndChannelClient *client)
{
    if (!client) {
        return;
    }

    reds_disable_mm_time(snd_channel_get_server(client));
    snd_channel_client_start(client);
}

SPICE_GNUC_VISIBLE void spice_server_playback_start(SpicePlaybackInstance *sin)
{
    SndChannel *channel = sin->st;
    channel->active = true;
    return playback_channel_client_start(snd_channel_get_client(channel));
}

SPICE_GNUC_VISIBLE void spice_server_playback_stop(SpicePlaybackInstance *sin)
{
    SndChannelClient *client = snd_channel_get_client(sin->st);

    sin->st->active = false;
    if (!client)
        return;
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(client);
    spice_assert(client->active);
    reds_enable_mm_time(snd_channel_get_server(client));
    client->active = false;
    if (client->client_active) {
        snd_set_command(client, SND_CTRL_MASK);
        snd_send(client);
    } else {
        client->command &= ~SND_CTRL_MASK;
        client->command &= ~SND_PLAYBACK_PCM_MASK;

        if (playback_client->pending_frame) {
            spice_assert(!playback_client->in_progress);
            snd_playback_free_frame(playback_client,
                                    playback_client->pending_frame);
            playback_client->pending_frame = nullptr;
        }
    }
}

SPICE_GNUC_VISIBLE void spice_server_playback_get_buffer(SpicePlaybackInstance *sin,
                                                         uint32_t **frame, uint32_t *num_samples)
{
    SndChannelClient *client = snd_channel_get_client(sin->st);

    *frame = nullptr;
    *num_samples = 0;
    if (!client) {
        return;
    }
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(client);
    if (!playback_client->free_frames) {
        return;
    }
    spice_assert(client->active);
    if (!playback_client->free_frames->allocated) {
        playback_client->free_frames->allocated = true;
        ++playback_client->frames->refs;
    }

    *frame = playback_client->free_frames->samples;
    playback_client->free_frames = playback_client->free_frames->next;
    *num_samples = snd_codec_frame_size(playback_client->codec);
}

SPICE_GNUC_VISIBLE void spice_server_playback_put_samples(SpicePlaybackInstance *sin, uint32_t *samples)
{
    PlaybackChannelClient *playback_client;
    AudioFrame *frame;

    frame = SPICE_CONTAINEROF(samples, AudioFrame, samples[0]);
    if (frame->allocated) {
        frame->allocated = false;
        if (--frame->container->refs == 0) {
            g_free(frame->container);
            return;
        }
    }
    playback_client = frame->client;
    if (!playback_client || snd_channel_get_client(sin->st) != playback_client) {
        /* lost last reference, client has been destroyed previously */
        spice_debug("audio samples belong to a disconnected client");
        return;
    }
    spice_assert(playback_client->active);

    if (playback_client->pending_frame) {
        snd_playback_free_frame(playback_client, playback_client->pending_frame);
    }
    frame->time = reds_get_mm_time();
    playback_client->pending_frame = frame;
    snd_set_command(playback_client, SND_PLAYBACK_PCM_MASK);
    snd_send(playback_client);
}

void snd_set_playback_latency(RedClient *client, uint32_t latency)
{
    GList *l;

    for (l = snd_channels; l != nullptr; l = l->next) {
        auto now = (SndChannel*) l->data;
        SndChannelClient *scc = snd_channel_get_client(now);
        if (now->type() == SPICE_CHANNEL_PLAYBACK && scc &&
            scc->get_client() == client) {

            if (scc->test_remote_cap(SPICE_PLAYBACK_CAP_LATENCY)) {
                auto  playback = (PlaybackChannelClient*)scc;

                playback->latency = latency;
                snd_set_command(scc, SND_PLAYBACK_LATENCY_MASK);
                snd_send(scc);
            } else {
                spice_debug("client doesn't not support SPICE_PLAYBACK_CAP_LATENCY");
            }
        }
    }
}

static int snd_desired_audio_mode(bool playback_compression, int frequency,
                                  bool client_can_opus)
{
    if (!playback_compression)
        return SPICE_AUDIO_DATA_MODE_RAW;

    if (client_can_opus && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, frequency))
        return SPICE_AUDIO_DATA_MODE_OPUS;

    return SPICE_AUDIO_DATA_MODE_RAW;
}

PlaybackChannelClient::~PlaybackChannelClient()
{
    int i;

    // free frames, unref them
    for (i = 0; i < NUM_AUDIO_FRAMES; ++i) {
        frames->items[i].client = nullptr;
    }
    if (--frames->refs == 0) {
        g_free(frames);
    }

    if (active) {
        reds_enable_mm_time(snd_channel_get_server(this));
    }

    snd_codec_destroy(&codec);
}


PlaybackChannelClient::PlaybackChannelClient(PlaybackChannel *channel,
                                             RedClient *client,
                                             RedStream *stream,
                                             RedChannelCapabilities *caps):
    SndChannelClient(channel, client, stream, caps)
{
    snd_playback_alloc_frames(this);

    bool client_can_opus = test_remote_cap(SPICE_PLAYBACK_CAP_OPUS);
    bool playback_compression =
        reds_config_get_playback_compression(channel->get_server());
    int desired_mode = snd_desired_audio_mode(playback_compression, channel->frequency, client_can_opus);
    if (desired_mode != SPICE_AUDIO_DATA_MODE_RAW) {
        if (snd_codec_create(&codec, (SpiceAudioDataMode) desired_mode, channel->frequency,
                             SND_CODEC_ENCODE) == SND_CODEC_OK) {
            mode = desired_mode;
        } else {
            red_channel_warning(channel, "create encoder failed");
        }
    }

    spice_debug("playback client %p using mode %s", this,
                spice_audio_data_mode_to_string(mode));
}

bool PlaybackChannelClient::init()
{
    RedClient *red_client = get_client();
    SndChannelClient *scc = this;
    SndChannel *channel = get_channel();

    if (!SndChannelClient::init()) {
        return false;
    }

    if (!red_client->during_migrate_at_target()) {
        snd_set_command(scc, SND_PLAYBACK_MODE_MASK);
        if (channel->volume.volume_nchannels) {
            snd_set_command(scc, SND_VOLUME_MUTE_MASK);
        }
    }

    if (channel->active) {
        playback_channel_client_start(scc);
    }
    snd_send(scc);

    return true;
}

void SndChannel::set_peer_common()
{
    SndChannelClient *snd_client = snd_channel_get_client(this);

    /* sound channels currently only support a single client */
    if (snd_client) {
        snd_client->disconnect();
    }
}

void PlaybackChannel::on_connect(RedClient *client, RedStream *stream,
                                 int migration, RedChannelCapabilities *caps)
{
    set_peer_common();

    auto peer =
        red::make_shared<PlaybackChannelClient>(this, client, stream, caps);
    peer->init();
}

void SndChannelClient::migrate()
{
    snd_set_command(this, SND_MIGRATE_MASK);
    snd_send(this);
}

SPICE_GNUC_VISIBLE void spice_server_record_set_volume(SpiceRecordInstance *sin,
                                                uint8_t nchannels,
                                                uint16_t *volume)
{
    snd_channel_set_volume(sin->st, nchannels, volume);
}

SPICE_GNUC_VISIBLE void spice_server_record_set_mute(SpiceRecordInstance *sin, uint8_t mute)
{
    snd_channel_set_mute(sin->st, mute);
}

static void record_channel_client_start(SndChannelClient *client)
{
    if (!client) {
        return;
    }

    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(client);
    record_client->read_pos = record_client->write_pos = 0;   //todo: improve by
                                                              //stream generation
    snd_channel_client_start(client);
}

SPICE_GNUC_VISIBLE void spice_server_record_start(SpiceRecordInstance *sin)
{
    SndChannel *channel = sin->st;
    channel->active = true;
    record_channel_client_start(snd_channel_get_client(channel));
}

SPICE_GNUC_VISIBLE void spice_server_record_stop(SpiceRecordInstance *sin)
{
    SndChannelClient *client = snd_channel_get_client(sin->st);

    sin->st->active = false;
    if (!client)
        return;
    spice_assert(client->active);
    client->active = false;
    if (client->client_active) {
        snd_set_command(client, SND_CTRL_MASK);
        snd_send(client);
    } else {
        client->command &= ~SND_CTRL_MASK;
    }
}

SPICE_GNUC_VISIBLE uint32_t spice_server_record_get_samples(SpiceRecordInstance *sin,
                                                            uint32_t *samples, uint32_t bufsize)
{
    SndChannelClient *client = snd_channel_get_client(sin->st);
    uint32_t read_pos;
    uint32_t now;
    uint32_t len;

    if (!client)
        return 0;
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(client);
    spice_assert(client->active);

    if (record_client->write_pos < RECORD_SAMPLES_SIZE / 2) {
        return 0;
    }

    len = MIN(record_client->write_pos - record_client->read_pos, bufsize);

    read_pos = record_client->read_pos % RECORD_SAMPLES_SIZE;
    record_client->read_pos += len;
    now = MIN(len, RECORD_SAMPLES_SIZE - read_pos);
    memcpy(samples, &record_client->samples[read_pos], now * 4);
    if (now < len) {
        memcpy(samples + now, record_client->samples, (len - now) * 4);
    }
    return len;
}

static void snd_set_rate(SndChannel *channel, uint32_t frequency, uint32_t cap_opus)
{
    channel->frequency = frequency;
    if (channel && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, frequency)) {
        channel->set_cap(cap_opus);
    }
}

SPICE_GNUC_VISIBLE uint32_t spice_server_get_best_playback_rate(SpicePlaybackInstance *sin)
{
    return SND_CODEC_OPUS_PLAYBACK_FREQ;
}

SPICE_GNUC_VISIBLE void spice_server_set_playback_rate(SpicePlaybackInstance *sin, uint32_t frequency)
{
    snd_set_rate(sin->st, frequency, SPICE_PLAYBACK_CAP_OPUS);
}

SPICE_GNUC_VISIBLE uint32_t spice_server_get_best_record_rate(SpiceRecordInstance *sin)
{
    return SND_CODEC_OPUS_PLAYBACK_FREQ;
}

SPICE_GNUC_VISIBLE void spice_server_set_record_rate(SpiceRecordInstance *sin, uint32_t frequency)
{
    snd_set_rate(sin->st, frequency, SPICE_RECORD_CAP_OPUS);
}

RecordChannelClient::~RecordChannelClient()
{
    snd_codec_destroy(&codec);
}

bool RecordChannelClient::init()
{
    SndChannel *channel = get_channel();

    if (!SndChannelClient::init()) {
        return FALSE;
    }

    if (channel->volume.volume_nchannels) {
        snd_set_command(this, SND_VOLUME_MUTE_MASK);
    }

    if (channel->active) {
        record_channel_client_start(this);
    }
    snd_send(this);

    return TRUE;
}

void RecordChannel::on_connect(RedClient *client, RedStream *stream,
                               int migration, RedChannelCapabilities *caps)
{
    set_peer_common();

    auto peer =
        red::make_shared<RecordChannelClient>(this, client, stream, caps);
    peer->init();
}

static void add_channel(SndChannel *channel)
{
    snd_channels = g_list_prepend(snd_channels, channel);
}

static void remove_channel(SndChannel *channel)
{
    snd_channels = g_list_remove(snd_channels, channel);
}

SndChannel::~SndChannel()
{
    remove_channel(this);

    g_free(volume.volume);
    volume.volume = nullptr;
}

PlaybackChannel::PlaybackChannel(RedsState *reds):
    SndChannel(reds, SPICE_CHANNEL_PLAYBACK, 0)
{
    set_cap(SPICE_PLAYBACK_CAP_VOLUME);

    add_channel(this);
    reds_register_channel(reds, this);
}

void snd_attach_playback(RedsState *reds, SpicePlaybackInstance *sin)
{
    sin->st = new PlaybackChannel(reds); // XXX make_shared
}

RecordChannel::RecordChannel(RedsState *reds):
    SndChannel(reds, SPICE_CHANNEL_RECORD, 0)
{
    set_cap(SPICE_RECORD_CAP_VOLUME);

    add_channel(this);
    reds_register_channel(reds, this);
}

void snd_attach_record(RedsState *reds, SpiceRecordInstance *sin)
{
    sin->st = new RecordChannel(reds); // XXX make_shared
}

static void snd_detach_common(SndChannel *channel)
{
    if (!channel) {
        return;
    }

    channel->destroy();
}

void snd_detach_playback(SpicePlaybackInstance *sin)
{
    snd_detach_common(sin->st);
}

void snd_detach_record(SpiceRecordInstance *sin)
{
    snd_detach_common(sin->st);
}

void snd_set_playback_compression(bool on)
{
    GList *l;

    for (l = snd_channels; l != nullptr; l = l->next) {
        auto now = (SndChannel*) l->data;
        SndChannelClient *client = snd_channel_get_client(now);
        if (now->type() == SPICE_CHANNEL_PLAYBACK && client) {
            PlaybackChannelClient* playback = PLAYBACK_CHANNEL_CLIENT(client);
            RedChannelClient *rcc = playback;
            bool client_can_opus = rcc->test_remote_cap(SPICE_PLAYBACK_CAP_OPUS);
            int desired_mode = snd_desired_audio_mode(on, now->frequency, client_can_opus);
            if (playback->mode != desired_mode) {
                playback->mode = desired_mode;
                snd_set_command(client, SND_PLAYBACK_MODE_MASK);
                spice_debug("playback client %p using mode %s", playback,
                            spice_audio_data_mode_to_string(playback->mode));
            }
        }
    }
}

static void snd_playback_alloc_frames(PlaybackChannelClient *playback)
{
    int i;

    playback->frames = g_new0(AudioFrameContainer, 1);
    playback->frames->refs = 1;
    for (i = 0; i < NUM_AUDIO_FRAMES; ++i) {
        playback->frames->items[i].container = playback->frames;
        snd_playback_free_frame(playback, &playback->frames->items[i]);
    }
}
