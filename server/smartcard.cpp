/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2010-2016 Red Hat, Inc.

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

#include <arpa/inet.h>
#ifdef USE_SMARTCARD
#include <libcacard.h>
#endif

#include "reds.h"
#include "char-device.h"
#include "smartcard.h"
#include "smartcard-channel-client.h"
#include "migration-protocol.h"

/*
 * TODO: the code doesn't really support multiple readers.
 * For example: smartcard_char_device_add_to_readers calls smartcard_init,
 * which can be called only once.
 * We should allow different readers, at least one reader per client.
 * In addition the implementation should be changed: instead of one channel for
 * all readers, we need to have different channles for different readers,
 * similarly to spicevmc.
 *
 */
#define SMARTCARD_MAX_READERS 10

// Maximal length of APDU
#define APDUBufSize 270

struct RedSmartcardChannel final: public RedChannel
{
    RedSmartcardChannel(RedsState *reds);
    void on_connect(RedClient *client, RedStream *stream, int migration,
                    RedChannelCapabilities *caps) override;
};

struct RedCharDeviceSmartcardPrivate {
    SPICE_CXX_GLIB_ALLOCATOR

    RedCharDeviceSmartcardPrivate();
    ~RedCharDeviceSmartcardPrivate();

    uint32_t             reader_id;
    /* read_from_device buffer */
    uint8_t             *buf;
    uint32_t             buf_size;
    uint8_t             *buf_pos;
    uint32_t             buf_used;

    SmartCardChannelClient    *scc; // client providing the remote card
    int                  reader_added; // has reader_add been sent to the device
};

struct RedMsgItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_SMARTCARD_DATA> {
    red::glib_unique_ptr<VSCMsgHeader> vheader;
};

static red::shared_ptr<RedMsgItem>
smartcard_new_vsc_msg_item(unsigned int reader_id, const VSCMsgHeader *vheader);

static struct Readers {
    uint32_t num;
    SpiceCharDeviceInstance* sin[SMARTCARD_MAX_READERS];
} g_smartcard_readers = {0, {NULL}};

static int smartcard_char_device_add_to_readers(RedsState *reds, SpiceCharDeviceInstance *sin);

static red::shared_ptr<RedMsgItem>
smartcard_char_device_on_message_from_device(RedCharDeviceSmartcard *dev, VSCMsgHeader *header);
static void smartcard_init(RedsState *reds);

static void smartcard_read_buf_prepare(RedCharDeviceSmartcard *dev, VSCMsgHeader *vheader)
{
    uint32_t msg_len;

    msg_len = ntohl(vheader->length);
    if (msg_len > dev->priv->buf_size) {
        dev->priv->buf_size = MAX(dev->priv->buf_size * 2, msg_len + sizeof(VSCMsgHeader));
        dev->priv->buf = (uint8_t*) g_realloc(dev->priv->buf, dev->priv->buf_size);
    }
}

RedPipeItemPtr
RedCharDeviceSmartcard::read_one_msg_from_device()
{
    RedCharDeviceSmartcard *dev = this;
    VSCMsgHeader *vheader = (VSCMsgHeader*)dev->priv->buf;
    int remaining;
    int actual_length;

    while (true) {
        // it's possible we already got a full message from a previous partial
        // read. In this case we don't need to read any byte
        if (dev->priv->buf_used < sizeof(VSCMsgHeader) ||
            dev->priv->buf_used - sizeof(VSCMsgHeader) < ntohl(vheader->length)) {
            int n = read(dev->priv->buf_pos, dev->priv->buf_size - dev->priv->buf_used);
            if (n <= 0) {
                break;
            }
            dev->priv->buf_pos += n;
            dev->priv->buf_used += n;

            if (dev->priv->buf_used < sizeof(VSCMsgHeader)) {
                continue;
            }
            smartcard_read_buf_prepare(dev, vheader);
            vheader = (VSCMsgHeader*)dev->priv->buf;
        }
        actual_length = ntohl(vheader->length);
        if (dev->priv->buf_used - sizeof(VSCMsgHeader) < actual_length) {
            continue;
        }
        auto msg_to_client = smartcard_char_device_on_message_from_device(dev, vheader);
        remaining = dev->priv->buf_used - sizeof(VSCMsgHeader) - actual_length;
        if (remaining > 0) {
            memmove(dev->priv->buf, dev->priv->buf_pos - remaining, remaining);
        }
        dev->priv->buf_pos = dev->priv->buf + remaining;
        dev->priv->buf_used = remaining;
        if (msg_to_client && dev->priv->scc) {
            dev->priv->scc->pipe_add_push(std::move(msg_to_client));
        }
    }
    return RedPipeItemPtr();
}

void RedCharDeviceSmartcard::remove_client(RedCharDeviceClientOpaque *opaque)
{
    SmartCardChannelClient *scc = (SmartCardChannelClient *) opaque;

    spice_assert(priv->scc && priv->scc == scc);
    scc->shutdown();
}

red::shared_ptr<RedMsgItem>
smartcard_char_device_on_message_from_device(RedCharDeviceSmartcard *dev,
                                             VSCMsgHeader *vheader)
{
    vheader->type = ntohl(vheader->type);
    vheader->length = ntohl(vheader->length);
    vheader->reader_id = ntohl(vheader->reader_id);

    if (vheader->type == VSC_Init) {
        return red::shared_ptr<RedMsgItem>();
    }
    /* We pass any VSC_Error right now - might need to ignore some? */
    if (dev->priv->reader_id == VSCARD_UNDEFINED_READER_ID) {
        red_channel_warning(dev->priv->scc->get_channel(),
                            "error: reader_id not assigned for message of type %d",
                            vheader->type);
    }
    if (dev->priv->scc == NULL) {
        return red::shared_ptr<RedMsgItem>();
    }
    return smartcard_new_vsc_msg_item(dev->priv->reader_id, vheader);
}

XXX_CAST(RedCharDevice, RedCharDeviceSmartcard, RED_CHAR_DEVICE_SMARTCARD);

static int smartcard_char_device_add_to_readers(RedsState *reds, SpiceCharDeviceInstance *char_device)
{
    RedCharDeviceSmartcard *dev = RED_CHAR_DEVICE_SMARTCARD(char_device->st);

    if (g_smartcard_readers.num >= SMARTCARD_MAX_READERS) {
        return -1;
    }
    dev->priv->reader_id = g_smartcard_readers.num;
    g_smartcard_readers.sin[g_smartcard_readers.num++] = char_device;
    smartcard_init(reds);
    return 0;
}

SpiceCharDeviceInstance *smartcard_readers_get(uint32_t reader_id)
{
    if (reader_id >= g_smartcard_readers.num) {
        return NULL;
    }
    return g_smartcard_readers.sin[reader_id];
}

/* TODO: fix implementation for multiple readers. Each reader should have a separated
 * channel */
SpiceCharDeviceInstance *smartcard_readers_get_unattached(void)
{
    int i;
    RedCharDeviceSmartcard* dev;

    for (i = 0; i < g_smartcard_readers.num; ++i) {
        dev = RED_CHAR_DEVICE_SMARTCARD(g_smartcard_readers.sin[i]->st);
        if (!dev->priv->scc) {
            return g_smartcard_readers.sin[i];
        }
    }
    return NULL;
}

RedCharDeviceSmartcardPrivate::RedCharDeviceSmartcardPrivate()
{
    reader_id = VSCARD_UNDEFINED_READER_ID;
    buf_size = APDUBufSize + sizeof(VSCMsgHeader);
    buf = (uint8_t*) g_malloc(buf_size);
    buf_pos = buf;
}

RedCharDeviceSmartcardPrivate::~RedCharDeviceSmartcardPrivate()
{
    g_free(buf);
}

RedCharDeviceSmartcard::RedCharDeviceSmartcard(RedsState *reds, SpiceCharDeviceInstance *sin):
    RedCharDevice(reds, sin, 0, ~0ULL)
{
}

red::shared_ptr<RedCharDeviceSmartcard> smartcard_device_connect(RedsState *reds, SpiceCharDeviceInstance *char_device)
{
    auto dev =
        red::make_shared<RedCharDeviceSmartcard>(reds, char_device);

    if (smartcard_char_device_add_to_readers(reds, char_device) == -1) {
        dev.reset();
    }
    return dev;
}

void smartcard_char_device_notify_reader_add(RedCharDeviceSmartcard *dev)
{
    RedCharDeviceWriteBuffer *write_buf;
    VSCMsgHeader *vheader;

    write_buf = dev->write_buffer_get_server(sizeof(*vheader), true);
    if (!write_buf) {
        spice_error("failed to allocate write buffer");
        return;
    }
    dev->priv->reader_added = TRUE;
    vheader = (VSCMsgHeader *)write_buf->buf;
    vheader->type = VSC_ReaderAdd;
    vheader->reader_id = dev->priv->reader_id;
    vheader->length = 0;
    smartcard_channel_write_to_reader(write_buf);
}

void smartcard_char_device_attach_client(SpiceCharDeviceInstance *char_device,
                                         SmartCardChannelClient *scc)
{
    RedCharDeviceSmartcard *dev = RED_CHAR_DEVICE_SMARTCARD(char_device->st);
    int client_added;

    spice_assert(!smartcard_channel_client_get_char_device(scc) && !dev->priv->scc);
    dev->priv->scc = scc;
    smartcard_channel_client_set_char_device(scc, dev);
    client_added = dev->client_add((RedCharDeviceClientOpaque *)scc, FALSE, 0,
                                   ~0, ~0, scc->is_waiting_for_migrate_data());
    if (!client_added) {
        spice_warning("failed");
        dev->priv->scc = NULL;
        smartcard_channel_client_set_char_device(scc, NULL);
        scc->disconnect();
    } else {
        SpiceCharDeviceInterface *sif = spice_char_device_get_interface(char_device);
        if (sif->state) {
            sif->state(char_device, 1);
        }
    }
}

gboolean smartcard_char_device_notify_reader_remove(RedCharDeviceSmartcard *dev)
{
    RedCharDeviceWriteBuffer *write_buf;
    VSCMsgHeader *vheader;

    if (!dev->priv->reader_added) {
        spice_debug("reader add was never sent to the device");
        return FALSE;
    }
    write_buf = dev->write_buffer_get_server(sizeof(*vheader), true);
    if (!write_buf) {
        spice_error("failed to allocate write buffer");
        return FALSE;
    }
    dev->priv->reader_added = FALSE;
    vheader = (VSCMsgHeader *)write_buf->buf;
    vheader->type = VSC_ReaderRemove;
    vheader->reader_id = dev->priv->reader_id;
    vheader->length = 0;
    smartcard_channel_write_to_reader(write_buf);

    return TRUE;
}

void smartcard_char_device_detach_client(RedCharDeviceSmartcard *smartcard,
                                         SmartCardChannelClient *scc)
{
    SpiceCharDeviceInterface *sif;
    SpiceCharDeviceInstance *sin;

    sin = smartcard->get_device_instance();
    sif = spice_char_device_get_interface(sin);

    spice_assert(smartcard->priv->scc == scc);
    smartcard->client_remove((RedCharDeviceClientOpaque *)scc);
    smartcard_channel_client_set_char_device(scc, NULL);
    smartcard->priv->scc = NULL;

    if (sif->state) {
        sif->state(sin, 0);
    }
}

SmartCardChannelClient* smartcard_char_device_get_client(RedCharDeviceSmartcard *smartcard)
{
    return smartcard->priv->scc;
}

static void smartcard_channel_send_msg(RedChannelClient *rcc,
                                       SpiceMarshaller *m, RedPipeItem *item)
{
    RedMsgItem* msg_item = static_cast<RedMsgItem*>(item);

    smartcard_channel_client_send_data(rcc, m, item, msg_item->vheader.get());
}

static void smartcard_channel_send_migrate_data(SmartCardChannelClient *scc,
                                                SpiceMarshaller *m, RedPipeItem *item)
{
    SpiceMarshaller *m2;

    auto dev = smartcard_channel_client_get_char_device(scc);
    scc->init_send_data(SPICE_MSG_MIGRATE_DATA);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_SMARTCARD_MAGIC);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_SMARTCARD_VERSION);

    if (!dev) {
        RedCharDevice::migrate_data_marshall_empty(m);
        spice_marshaller_add_uint8(m, 0);
        spice_marshaller_add_uint32(m, 0);
        spice_marshaller_add_uint32(m, 0);
        spice_debug("null char dev");
    } else {
        dev->migrate_data_marshall(m);
        spice_marshaller_add_uint8(m, dev->priv->reader_added);
        spice_marshaller_add_uint32(m, dev->priv->buf_used);
        m2 = spice_marshaller_get_ptr_submarshaller(m);
        spice_marshaller_add(m2, dev->priv->buf, dev->priv->buf_used);
        spice_debug("reader added %d partial read size %u", dev->priv->reader_added, dev->priv->buf_used);
    }
}

void SmartCardChannelClient::send_item(RedPipeItem *item)
{
    SpiceMarshaller *m = get_marshaller();

    switch (item->type) {
    case RED_PIPE_ITEM_TYPE_ERROR:
        smartcard_channel_client_send_error(this, m, item);
        break;
    case RED_PIPE_ITEM_TYPE_SMARTCARD_DATA:
        smartcard_channel_send_msg(this, m, item);
        break;
    case RED_PIPE_ITEM_TYPE_SMARTCARD_MIGRATE_DATA:
        smartcard_channel_send_migrate_data(this, m, item);
        break;
    default:
        spice_error("bad pipe item %d", item->type);
        return;
    }
    begin_send_message();
}

static red::shared_ptr<RedMsgItem>
smartcard_new_vsc_msg_item(unsigned int reader_id, const VSCMsgHeader *vheader)
{
    auto msg_item = red::make_shared<RedMsgItem>();

    msg_item->vheader.reset((VSCMsgHeader*) g_memdup(vheader, sizeof(*vheader) + vheader->length));
    /* We patch the reader_id, since the device only knows about itself, and
     * we know about the sum of readers. */
    msg_item->vheader->reader_id = reader_id;
    return msg_item;
}

void smartcard_channel_write_to_reader(RedCharDeviceWriteBuffer *write_buf)
{
    SpiceCharDeviceInstance *sin;
    RedCharDeviceSmartcard *dev;
    VSCMsgHeader *vheader;
    uint32_t actual_length;

    vheader = (VSCMsgHeader *)write_buf->buf;
    actual_length = vheader->length;

    spice_assert(vheader->reader_id <= g_smartcard_readers.num);
    sin = g_smartcard_readers.sin[vheader->reader_id];
    dev = RED_CHAR_DEVICE_SMARTCARD(sin->st);
    spice_assert(!dev->priv->scc ||
                 dev == smartcard_channel_client_get_char_device(dev->priv->scc).get());
    /* protocol requires messages to be in network endianness */
    vheader->type = htonl(vheader->type);
    vheader->length = htonl(vheader->length);
    vheader->reader_id = htonl(vheader->reader_id);
    write_buf->buf_used = actual_length + sizeof(VSCMsgHeader);
    /* pushing the buffer to the write queue; It will be released
     * when it will be fully consumed by the device */
    sin->st->write_buffer_add(write_buf);
}

static void smartcard_device_restore_partial_read(RedCharDeviceSmartcard *dev,
                                                  SpiceMigrateDataSmartcard *mig_data)
{
    uint8_t *read_data;

    spice_debug("read_size  %u", mig_data->read_size);
    read_data = (uint8_t *)mig_data + mig_data->read_data_ptr - sizeof(SpiceMigrateDataHeader);
    if (mig_data->read_size < sizeof(VSCMsgHeader)) {
        spice_assert(dev->priv->buf_size >= mig_data->read_size);
    } else {
        smartcard_read_buf_prepare(dev, (VSCMsgHeader *)read_data);
    }
    memcpy(dev->priv->buf, read_data, mig_data->read_size);
    dev->priv->buf_used = mig_data->read_size;
    dev->priv->buf_pos = dev->priv->buf + mig_data->read_size;
}

int smartcard_char_device_handle_migrate_data(RedCharDeviceSmartcard *smartcard,
                                              SpiceMigrateDataSmartcard *mig_data)
{
    smartcard->priv->reader_added = mig_data->reader_added;

    smartcard_device_restore_partial_read(smartcard, mig_data);
    return smartcard->restore(&mig_data->base);
}

void RedSmartcardChannel::on_connect(RedClient *client, RedStream *stream, int migration,
                                     RedChannelCapabilities *caps)
{
    SpiceCharDeviceInstance *char_device =
            smartcard_readers_get_unattached();

    auto scc = smartcard_channel_client_create(this, client, stream, caps);

    if (!scc) {
        return;
    }
    scc->ack_zero_messages_window();

    if (char_device) {
        smartcard_char_device_attach_client(char_device, scc);
    } else {
        red_channel_warning(this, "char dev unavailable");
    }
}

RedSmartcardChannel::RedSmartcardChannel(RedsState *reds):
    RedChannel(reds, SPICE_CHANNEL_SMARTCARD, 0, RedChannel::MigrateAll)
{
    reds_register_channel(reds, this);
}

static void smartcard_init(RedsState *reds)
{
    spice_assert(!reds_find_channel(reds, SPICE_CHANNEL_SMARTCARD, 0));

    red::make_shared<RedSmartcardChannel>(reds);
}


RedCharDeviceSmartcard::~RedCharDeviceSmartcard()
{
}

uint32_t smartcard_get_n_readers(void)
{
    return g_smartcard_readers.num;
}
