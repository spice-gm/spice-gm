/* spice-server spicevmc passthrough channel code

   Copyright (C) 2011 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

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

#include <assert.h>
#include <string.h>
#ifdef USE_LZ4
#include <lz4.h>
#endif

#include <common/generated_server_marshallers.h>

#include "char-device.h"
#include "red-channel.h"
#include "red-channel-client.h"
#include "reds.h"
#include "migration-protocol.h"

/* 64K should be enough for all but the largest writes + 32 bytes hdr */
#define BUF_SIZE (64 * 1024 + 32)
#define COMPRESS_THRESHOLD 1000

// limit of the queued data, at this limit we stop reading from device to
// avoid DoS
#define QUEUED_DATA_LIMIT (1024*1024)

enum {
    RED_PIPE_ITEM_TYPE_SPICEVMC_DATA = RED_PIPE_ITEM_TYPE_CHANNEL_BASE,
    RED_PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA,
    RED_PIPE_ITEM_TYPE_PORT_INIT,
    RED_PIPE_ITEM_TYPE_PORT_EVENT,
};

struct RedVmcChannel;
class VmcChannelClient;

struct RedVmcPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_SPICEVMC_DATA> {
    SpiceDataCompressionType type;
    uint32_t uncompressed_data_size = 0;
    /* writes which don't fit this will get split, this is not a problem */
    uint8_t buf[BUF_SIZE];
    uint32_t buf_used = 0;
};

struct RedCharDeviceSpiceVmc: public RedCharDevice
{
    RedCharDeviceSpiceVmc(SpiceCharDeviceInstance *sin, RedsState *reds, RedVmcChannel *channel);
    ~RedCharDeviceSpiceVmc() override;

    RedPipeItemPtr read_one_msg_from_device() override;
    void remove_client(RedCharDeviceClientOpaque *opaque) override;
    void on_free_self_token() override;
    void port_event(uint8_t event) override;

    red::shared_ptr<RedVmcChannel> channel;
};

static void spicevmc_red_channel_queue_data(RedVmcChannel *channel, red::shared_ptr<RedVmcPipeItem>&& item);

struct RedVmcChannel: public RedChannel
{
    RedVmcChannel(RedsState *reds, uint32_t type, uint32_t id);
    ~RedVmcChannel() override;

    void on_connect(RedClient *client, RedStream *stream, int migration,
                    RedChannelCapabilities *caps) override;

    VmcChannelClient *rcc;
    RedCharDevice *chardev; /* weak */
    SpiceCharDeviceInstance *chardev_sin;
    red::shared_ptr<RedVmcPipeItem> pipe_item;
    RedCharDeviceWriteBuffer *recv_from_client_buf;
    uint8_t port_opened;
    uint32_t queued_data;
    RedStatCounter in_data;
    RedStatCounter in_compressed;
    RedStatCounter in_decompressed;
    RedStatCounter out_data;
    RedStatCounter out_compressed;
    RedStatCounter out_uncompressed;
};


class VmcChannelClient final: public RedChannelClient
{
    using RedChannelClient::RedChannelClient;
public:
    RedVmcChannel* get_channel()
    {
        return static_cast<RedVmcChannel*>(RedChannelClient::get_channel());
    }
protected:
    uint8_t *alloc_recv_buf(uint16_t type, uint32_t size) override;
    void release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg) override;
    void on_disconnect() override;
    bool handle_message(uint16_t type, uint32_t size, void *msg) override;
    void send_item(RedPipeItem *item) override;
    bool handle_migrate_data(uint32_t size, void *message) override;
    void handle_migrate_flush_mark() override;
};

static VmcChannelClient *
vmc_channel_client_create(RedChannel *channel, RedClient *client,
                          RedStream *stream,
                          RedChannelCapabilities *caps);


RedVmcChannel::RedVmcChannel(RedsState *reds, uint32_t type, uint32_t id):
    RedChannel(reds, type, id, RedChannel::MigrateAll)
{
    init_stat_node(nullptr, "spicevmc");
    const RedStatNode *stat = get_stat_node();
    stat_init_counter(&in_data, reds, stat, "in_data", TRUE);
    stat_init_counter(&in_compressed, reds, stat, "in_compressed", TRUE);
    stat_init_counter(&in_decompressed, reds, stat, "in_decompressed", TRUE);
    stat_init_counter(&out_data, reds, stat, "out_data", TRUE);
    stat_init_counter(&out_compressed, reds, stat, "out_compressed", TRUE);
    stat_init_counter(&out_uncompressed, reds, stat, "out_uncompressed", TRUE);

#ifdef USE_LZ4
    set_cap(SPICE_SPICEVMC_CAP_DATA_COMPRESS_LZ4);
#endif

    reds_register_channel(reds, this);
}

RedVmcChannel::~RedVmcChannel()
{
    RedCharDevice::write_buffer_release(chardev, &recv_from_client_buf);
}

static red::shared_ptr<RedVmcChannel> red_vmc_channel_new(RedsState *reds, uint8_t channel_type)
{
    switch (channel_type) {
        case SPICE_CHANNEL_USBREDIR:
        case SPICE_CHANNEL_WEBDAV:
        case SPICE_CHANNEL_PORT:
            break;
        default:
            g_error("Unsupported channel_type for red_vmc_channel_new(): %u", channel_type);
            return red::shared_ptr<RedVmcChannel>();
    }

    int id = reds_get_free_channel_id(reds, channel_type);
    if (id < 0) {
        g_warning("Free ID not found creating new VMC channel");
        return red::shared_ptr<RedVmcChannel>();
    }

    return red::make_shared<RedVmcChannel>(reds, channel_type, id);
}

struct RedPortInitPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_PORT_INIT> {
    RedPortInitPipeItem(const char *name, uint8_t opened);

    red::glib_unique_ptr<char> name;
    uint8_t opened;
};

struct RedPortEventPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_PORT_EVENT> {
    uint8_t event;
};

/* msg_item -- the current pipe item with the uncompressed data
 * This function returns:
 *  - false upon failure.
 *  - true if compression succeeded
 */
static bool
try_compress_lz4(RedVmcChannel *channel, red::shared_ptr<RedVmcPipeItem>& msg_item)
{
#ifdef USE_LZ4
    int compressed_data_count;
    auto n = msg_item->buf_used;

    if (red_stream_get_family(channel->rcc->get_stream()) == AF_UNIX) {
        /* AF_LOCAL - data will not be compressed */
        return false;
    }
    if (n <= COMPRESS_THRESHOLD) {
        /* n <= threshold - data will not be compressed */
        return false;
    }
    if (!channel->rcc->test_remote_cap(SPICE_SPICEVMC_CAP_DATA_COMPRESS_LZ4)) {
        /* Client doesn't have compression cap - data will not be compressed */
        return false;
    }
    auto msg_item_compressed = red::make_shared<RedVmcPipeItem>();
    compressed_data_count = LZ4_compress_default((char*)&msg_item->buf,
                                                 (char*)&msg_item_compressed->buf,
                                                 n,
                                                 BUF_SIZE);

    if (compressed_data_count > 0 && compressed_data_count < n) {
        stat_inc_counter(channel->out_uncompressed, n);
        stat_inc_counter(channel->out_compressed, compressed_data_count);
        msg_item_compressed->type = SPICE_DATA_COMPRESSION_TYPE_LZ4;
        msg_item_compressed->uncompressed_data_size = n;
        msg_item_compressed->buf_used = compressed_data_count;
        msg_item = std::move(msg_item_compressed);
        return true;
    }

    /* LZ4 compression failed or did non compress, fallback a non-compressed data is to be sent */
#endif
    return false;
}

RedPipeItemPtr
RedCharDeviceSpiceVmc::read_one_msg_from_device()
{
    red::shared_ptr<RedVmcPipeItem> msg_item;
    int n;

    if (!channel->rcc || channel->queued_data >= QUEUED_DATA_LIMIT) {
        return RedPipeItemPtr();
    }

    if (!channel->pipe_item) {
        msg_item = red::make_shared<RedVmcPipeItem>();
        msg_item->type = SPICE_DATA_COMPRESSION_TYPE_NONE;
    } else {
        spice_assert(channel->pipe_item->buf_used == 0);
        msg_item = std::move(channel->pipe_item);
    }

    n = read(msg_item->buf, sizeof(msg_item->buf));
    if (n > 0) {
        spice_debug("read from dev %d", n);
        msg_item->uncompressed_data_size = n;
        msg_item->buf_used = n;

        if (!try_compress_lz4(channel.get(), msg_item)) {
            stat_inc_counter(channel->out_data, n);
        }
        spicevmc_red_channel_queue_data(channel.get(), std::move(msg_item));
        return RedPipeItemPtr();
    }
    channel->pipe_item = std::move(msg_item);
    return RedPipeItemPtr();
}

RedPortInitPipeItem::RedPortInitPipeItem(const char *init_name, uint8_t init_opened):
    name(g_strdup(init_name)),
    opened(init_opened)
{
}

static void spicevmc_port_send_init(VmcChannelClient *rcc)
{
    RedVmcChannel *channel = rcc->get_channel();
    SpiceCharDeviceInstance *sin = channel->chardev_sin;
    auto item = red::make_shared<RedPortInitPipeItem>(sin->portname, channel->port_opened);

    rcc->pipe_add_push(item);
}

static void spicevmc_port_send_event(RedChannelClient *rcc, uint8_t event)
{
    auto item = red::make_shared<RedPortEventPipeItem>();

    item->event = event;
    rcc->pipe_add_push(item);
}

void RedCharDeviceSpiceVmc::remove_client(RedCharDeviceClientOpaque *opaque)
{
    auto client = (RedClient *) opaque;

    spice_assert(channel->rcc &&
                 channel->rcc->get_client() == client);

    channel->rcc->shutdown();
}

void VmcChannelClient::on_disconnect()
{
    RedVmcChannel *channel;
    SpiceCharDeviceInterface *sif;
    RedClient *client = get_client();

    channel = get_channel();

    /* partial message which wasn't pushed to device */
    RedCharDevice::write_buffer_release(channel->chardev,
                                        &channel->recv_from_client_buf);

    if (channel->chardev) {
        if (channel->chardev->client_exists((RedCharDeviceClientOpaque *)client)) {
            channel->chardev->client_remove((RedCharDeviceClientOpaque *)client);
        } else {
            red_channel_warning(channel,
                                "client %p have already been removed from char dev %p",
                                client, channel->chardev);
        }
    }

    channel->rcc = nullptr;
    sif = spice_char_device_get_interface(channel->chardev_sin);
    if (sif->state) {
        sif->state(channel->chardev_sin, 0);
    }
}

void VmcChannelClient::handle_migrate_flush_mark()
{
    pipe_add_type(RED_PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA);
}

bool VmcChannelClient::handle_migrate_data(uint32_t size, void *message)
{
    SpiceMigrateDataHeader *header;
    SpiceMigrateDataSpiceVmc *mig_data;
    RedVmcChannel *channel;

    channel = get_channel();

    header = (SpiceMigrateDataHeader *)message;
    mig_data = (SpiceMigrateDataSpiceVmc *)(header + 1);
    spice_assert(size >= sizeof(SpiceMigrateDataHeader) + sizeof(SpiceMigrateDataSpiceVmc));

    if (!migration_protocol_validate_header(header,
                                            SPICE_MIGRATE_DATA_SPICEVMC_MAGIC,
                                            SPICE_MIGRATE_DATA_SPICEVMC_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }
    return channel->chardev->restore(&mig_data->base);
}

static bool handle_compressed_msg(RedVmcChannel *channel, RedChannelClient *rcc,
                                  SpiceMsgCompressedData *compressed_data_msg)
{
    /* NOTE: *decompressed is free by the char-device */
    int decompressed_size;
    RedCharDeviceWriteBuffer *write_buf;

    write_buf = channel->chardev->write_buffer_get_server(compressed_data_msg->uncompressed_size,
                                                          false);
    if (!write_buf) {
        return FALSE;
    }

    switch (compressed_data_msg->type) {
#ifdef USE_LZ4
    case SPICE_DATA_COMPRESSION_TYPE_LZ4: {
        uint8_t *decompressed = write_buf->buf;
        decompressed_size = LZ4_decompress_safe ((char *)compressed_data_msg->compressed_data,
                                                 (char *)decompressed,
                                                 compressed_data_msg->compressed_size,
                                                 compressed_data_msg->uncompressed_size);
        stat_inc_counter(channel->in_compressed, compressed_data_msg->compressed_size);
        stat_inc_counter(channel->in_decompressed, decompressed_size);
        break;
    }
#endif
    default:
        spice_warning("Invalid Compression Type");
        RedCharDevice::write_buffer_release(channel->chardev, &write_buf);
        return FALSE;
    }
    if (decompressed_size != compressed_data_msg->uncompressed_size) {
        spice_warning("Decompression Error");
        RedCharDevice::write_buffer_release(channel->chardev, &write_buf);
        return FALSE;
    }
    write_buf->buf_used = decompressed_size;
    channel->chardev->write_buffer_add(write_buf);
    return TRUE;
}

bool VmcChannelClient::handle_message(uint16_t type, uint32_t size, void *msg)
{
    /* NOTE: *msg free by g_free() (when cb to VmcChannelClient::release_recv_buf
     * with the compressed msg type) */
    RedVmcChannel *channel;
    SpiceCharDeviceInterface *sif;

    channel = get_channel();
    sif = spice_char_device_get_interface(channel->chardev_sin);

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA:
        spice_assert(channel->recv_from_client_buf->buf == msg);
        stat_inc_counter(channel->in_data, size);
        channel->recv_from_client_buf->buf_used = size;
        channel->chardev->write_buffer_add(channel->recv_from_client_buf);
        channel->recv_from_client_buf = nullptr;
        break;
    case SPICE_MSGC_SPICEVMC_COMPRESSED_DATA:
        return handle_compressed_msg(channel, this, (SpiceMsgCompressedData*)msg);
        break;
    case SPICE_MSGC_PORT_EVENT:
        if (size != sizeof(uint8_t)) {
            spice_warning("bad port event message size");
            return FALSE;
        }
        if (sif->base.minor_version >= 2 && sif->event != nullptr)
            sif->event(channel->chardev_sin, *(uint8_t*)msg);
        break;
    default:
        return RedChannelClient::handle_message(type, size, msg);
    }

    return TRUE;
}

/* if device manage to send some data attempt to unblock the channel */
void RedCharDeviceSpiceVmc::on_free_self_token()
{
    channel->rcc->unblock_read();
}

uint8_t *VmcChannelClient::alloc_recv_buf(uint16_t type, uint32_t size)
{

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA: {
        RedVmcChannel *channel = get_channel();

        assert(!channel->recv_from_client_buf);

        channel->recv_from_client_buf = channel->chardev->write_buffer_get_server(size,
                                                                                  true);
        if (!channel->recv_from_client_buf) {
            block_read();
            return nullptr;
        }
        return channel->recv_from_client_buf->buf;
    }

    default:
        return (uint8_t*) g_malloc(size);
    }

}

void VmcChannelClient::release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg)
{

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA: {
        RedVmcChannel *channel = get_channel();
        /* buffer wasn't pushed to device */
        RedCharDevice::write_buffer_release(channel->chardev,
                                            &channel->recv_from_client_buf);
        break;
    }
    default:
        g_free(msg);
    }
}

static void
spicevmc_red_channel_queue_data(RedVmcChannel *channel, red::shared_ptr<RedVmcPipeItem>&& item)
{
    channel->queued_data += item->buf_used;
    channel->rcc->pipe_add_push(item);
}

static void spicevmc_red_channel_send_data(VmcChannelClient *rcc,
                                           SpiceMarshaller *m,
                                           RedPipeItem *item)
{
    auto i = static_cast<RedVmcPipeItem*>(item);
    RedVmcChannel *channel = rcc->get_channel();

    /* for compatibility send using not compressed data message */
    if (i->type == SPICE_DATA_COMPRESSION_TYPE_NONE) {
        rcc->init_send_data(SPICE_MSG_SPICEVMC_DATA);
    } else {
        /* send as compressed */
        rcc->init_send_data(SPICE_MSG_SPICEVMC_COMPRESSED_DATA);
        SpiceMsgCompressedData compressed_msg = {
            .type = i->type,
            .uncompressed_size = i->uncompressed_data_size
        };
        spice_marshall_SpiceMsgCompressedData(m, &compressed_msg);
    }
    item->add_to_marshaller(m, i->buf, i->buf_used);

    // account for sent data and wake up device if was blocked
    uint32_t old_queued_data = channel->queued_data;
    channel->queued_data -= i->buf_used;
    if (channel->chardev &&
        old_queued_data >= QUEUED_DATA_LIMIT && channel->queued_data < QUEUED_DATA_LIMIT) {
        channel->chardev->wakeup();
    }
}

static void spicevmc_red_channel_send_migrate_data(VmcChannelClient *rcc,
                                                   SpiceMarshaller *m,
                                                   RedPipeItem *item)
{
    RedVmcChannel *channel;

    channel = rcc->get_channel();
    rcc->init_send_data(SPICE_MSG_MIGRATE_DATA);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_SPICEVMC_MAGIC);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_SPICEVMC_VERSION);

    channel->chardev->migrate_data_marshall(m);
}

static void spicevmc_red_channel_send_port_init(RedChannelClient *rcc,
                                                SpiceMarshaller *m,
                                                RedPipeItem *item)
{
    auto i = static_cast<RedPortInitPipeItem*>(item);
    SpiceMsgPortInit init;

    rcc->init_send_data(SPICE_MSG_PORT_INIT);
    init.name = (uint8_t *)i->name.get();
    init.name_size = strlen(i->name.get()) + 1;
    init.opened = i->opened;
    spice_marshall_msg_port_init(m, &init);
}

static void spicevmc_red_channel_send_port_event(RedChannelClient *rcc,
                                                 SpiceMarshaller *m,
                                                 RedPipeItem *item)
{
    auto i = static_cast<RedPortEventPipeItem*>(item);
    SpiceMsgPortEvent event;

    rcc->init_send_data(SPICE_MSG_PORT_EVENT);
    event.event = i->event;
    spice_marshall_msg_port_event(m, &event);
}

void VmcChannelClient::send_item(RedPipeItem *item)
{
    SpiceMarshaller *m = get_marshaller();

    switch (item->type) {
    case RED_PIPE_ITEM_TYPE_SPICEVMC_DATA:
        spicevmc_red_channel_send_data(this, m, item);
        break;
    case RED_PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA:
        spicevmc_red_channel_send_migrate_data(this, m, item);
        break;
    case RED_PIPE_ITEM_TYPE_PORT_INIT:
        spicevmc_red_channel_send_port_init(this, m, item);
        break;
    case RED_PIPE_ITEM_TYPE_PORT_EVENT:
        spicevmc_red_channel_send_port_event(this, m, item);
        break;
    default:
        spice_error("bad pipe item %d", item->type);
        return;
    }
    begin_send_message();
}


void RedVmcChannel::on_connect(RedClient *client, RedStream *stream, int migration,
                               RedChannelCapabilities *caps)
{
    RedVmcChannel *vmc_channel;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    vmc_channel = this;
    sin = vmc_channel->chardev_sin;

    if (rcc) {
        red_channel_warning(this,
                            "channel client (%p) already connected, refusing second connection",
                            rcc);
        // TODO: notify client in advance about the in use channel using
        // SPICE_MSG_MAIN_CHANNEL_IN_USE (for example)
        red_stream_free(stream);
        return;
    }

    rcc = vmc_channel_client_create(this, client, stream, caps);
    if (!rcc) {
        return;
    }
    vmc_channel->queued_data = 0;
    rcc->ack_zero_messages_window();

    if (strcmp(sin->subtype, "port") == 0) {
        spicevmc_port_send_init(rcc);
    }

    if (!vmc_channel->chardev->client_add((RedCharDeviceClientOpaque *)client, FALSE, 0, ~0, ~0, rcc->is_waiting_for_migrate_data())) {
        spice_warning("failed to add client to spicevmc");
        rcc->disconnect();
        return;
    }

    sif = spice_char_device_get_interface(sin);
    if (sif->state) {
        sif->state(sin, 1);
    }
}

red::shared_ptr<RedCharDevice>
spicevmc_device_connect(RedsState *reds, SpiceCharDeviceInstance *sin, uint8_t channel_type)
{
    auto channel(red_vmc_channel_new(reds, channel_type));
    if (!channel) {
        return red::shared_ptr<RedCharDevice>();
    }

    /* char device takes ownership of channel */
    auto dev = red::make_shared<RedCharDeviceSpiceVmc>(sin, reds, channel.get());

    channel->chardev_sin = sin;

    return dev;
}

void RedCharDeviceSpiceVmc::port_event(uint8_t event)
{
    if (event == SPICE_PORT_EVENT_OPENED) {
        channel->port_opened = TRUE;
    } else if (event == SPICE_PORT_EVENT_CLOSED) {
        channel->port_opened = FALSE;
    }

    if (channel->rcc == nullptr) {
        return;
    }

    spicevmc_port_send_event(channel->rcc, event);
}

RedCharDeviceSpiceVmc::RedCharDeviceSpiceVmc(SpiceCharDeviceInstance *sin, RedsState *reds,
                                             RedVmcChannel *init_channel):
    RedCharDevice(reds, sin, 0, 128),
    channel(init_channel)
{
    if (channel) {
        channel->chardev = this;
    }
}

RedCharDeviceSpiceVmc::~RedCharDeviceSpiceVmc()
{
    if (channel) {
        // prevent possible recursive calls
        channel->chardev = nullptr;

        // close all current connections and drop the reference
        channel->destroy();
    }
}

static VmcChannelClient *
vmc_channel_client_create(RedChannel *channel, RedClient *client,
                          RedStream *stream,
                          RedChannelCapabilities *caps)
{
    auto rcc = red::make_shared<VmcChannelClient>(channel, client, stream, caps);
    if (!rcc->init()) {
        return nullptr;
    }
    return rcc.get();
}
