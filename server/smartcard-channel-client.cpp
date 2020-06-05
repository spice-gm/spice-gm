/*
    Copyright (C) 2009-2015 Red Hat, Inc.

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

#include "smartcard-channel-client.h"

struct SmartCardChannelClientPrivate
{
    SPICE_CXX_GLIB_ALLOCATOR

    red::weak_ptr<RedCharDeviceSmartcard> smartcard;

    /* read_from_client/write_to_device buffer.
     * The beginning of the buffer should always be VSCMsgHeader*/
    RedCharDeviceWriteBuffer *write_buf = nullptr;
    /* was the client msg received into a RedCharDeviceWriteBuffer
     * or was it explicitly malloced */
    bool msg_in_write_buf = false;
};

struct RedErrorItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_ERROR> {
    VSCMsgHeader vheader;
    VSCMsgError  error;
};

SmartCardChannelClient::SmartCardChannelClient(RedChannel *channel,
                                               RedClient *client,
                                               RedStream *stream,
                                               RedChannelCapabilities *caps):
    RedChannelClient(channel, client, stream, caps)
{
}

SmartCardChannelClient::~SmartCardChannelClient()
{
}

SmartCardChannelClient* smartcard_channel_client_create(RedChannel *channel,
                                                        RedClient *client, RedStream *stream,
                                                        RedChannelCapabilities *caps)
{
    auto rcc =
        red::make_shared<SmartCardChannelClient>(channel, client, stream, caps);
    if (!rcc->init()) {
        return nullptr;
    }
    return rcc.get();
}

uint8_t *
SmartCardChannelClient::alloc_recv_buf(uint16_t type, uint32_t size)
{
    /* TODO: only one reader is actually supported. When we fix the code to support
     * multiple readers, we will probably associate different devices to
     * different channels */
    if (auto smartcard = priv->smartcard.lock()) {
        spice_assert(smartcard_get_n_readers() == 1);
        spice_assert(smartcard_char_device_get_client(smartcard.get()));
        spice_assert(!priv->write_buf);
        priv->write_buf =
            smartcard->write_buffer_get_client((RedCharDeviceClientOpaque *)this,
                                               size);

        if (!priv->write_buf) {
            spice_error("failed to allocate write buffer");
            return NULL;
        }
        priv->msg_in_write_buf = TRUE;
        return priv->write_buf->buf;
    }
    priv->msg_in_write_buf = FALSE;
    return (uint8_t *) g_malloc(size);
}

void
SmartCardChannelClient::release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg)
{
    /* todo: only one reader is actually supported. When we fix the code to support
     * multiple readers, we will porbably associate different devices to
     * differenc channels */

    if (!priv->msg_in_write_buf) {
        spice_assert(!priv->write_buf);
        g_free(msg);
    } else {
        if (priv->write_buf) { /* msg hasn't been pushed to the guest */
            spice_assert(priv->write_buf->buf == msg);
            auto smartcard = priv->smartcard.lock();
            RedCharDevice::write_buffer_release(smartcard.get(), &priv->write_buf);
        }
    }
}

void SmartCardChannelClient::on_disconnect()
{
    if (auto device = priv->smartcard.lock()) {
        smartcard_char_device_detach_client(device.get(), this);
        smartcard_char_device_notify_reader_remove(device.get());
    }
}

void smartcard_channel_client_send_data(RedChannelClient *rcc,
                                        SpiceMarshaller *m,
                                        RedPipeItem *item,
                                        VSCMsgHeader *vheader)
{
    spice_assert(rcc);
    spice_assert(vheader);
    rcc->init_send_data(SPICE_MSG_SMARTCARD_DATA);
    /* NOTE: 'vheader' is assumed to be owned by 'item' so we keep the pipe
     * item valid until the message is actually sent. */
    item->add_to_marshaller(m, (uint8_t*)vheader, sizeof(VSCMsgHeader) + vheader->length);
}

void smartcard_channel_client_send_error(RedChannelClient *rcc, SpiceMarshaller *m, RedPipeItem *item)
{
    RedErrorItem* error_item = static_cast<RedErrorItem*>(item);

    smartcard_channel_client_send_data(rcc, m, item, &error_item->vheader);
}

static void smartcard_channel_client_push_error(RedChannelClient *rcc,
                                                uint32_t reader_id,
                                                VSCErrorCode error)
{
    auto error_item = red::make_shared<RedErrorItem>();

    error_item->vheader.reader_id = reader_id;
    error_item->vheader.type = VSC_Error;
    error_item->vheader.length = sizeof(error_item->error);
    error_item->error.code = error;
    rcc->pipe_add_push(error_item);
}

static void smartcard_channel_client_add_reader(SmartCardChannelClient *scc)
{
    auto smartcard = scc->priv->smartcard.lock();
    if (!smartcard) { /* we already tried to attach a reader to the client
                                          when it connected */
        SpiceCharDeviceInstance *char_device = smartcard_readers_get_unattached();

        if (!char_device) {
            smartcard_channel_client_push_error(scc,
                                                VSCARD_UNDEFINED_READER_ID,
                                                VSC_CANNOT_ADD_MORE_READERS);
            return;
        }
        smartcard_char_device_attach_client(char_device, scc);
        smartcard = scc->priv->smartcard.lock();
    }
    smartcard_char_device_notify_reader_add(smartcard.get());
    // The device sends a VSC_Error message, we will let it through, no
    // need to send our own. We already set the correct reader_id, from
    // our RedCharDeviceSmartcard.
}

XXX_CAST(RedCharDevice, RedCharDeviceSmartcard, RED_CHAR_DEVICE_SMARTCARD);

static void smartcard_channel_client_remove_reader(SmartCardChannelClient *scc,
                                                   uint32_t reader_id)
{
    SpiceCharDeviceInstance *char_device = smartcard_readers_get(reader_id);
    RedCharDeviceSmartcard *dev;

    if (char_device == NULL) {
        smartcard_channel_client_push_error(scc,
                                            reader_id, VSC_GENERAL_ERROR);
        return;
    }

    dev = RED_CHAR_DEVICE_SMARTCARD(char_device->st);
    spice_assert(scc->priv->smartcard.lock().get() == dev);
    if (!smartcard_char_device_notify_reader_remove(dev)) {
        smartcard_channel_client_push_error(scc,
                                            reader_id, VSC_GENERAL_ERROR);
        return;
    }
}

static void smartcard_channel_client_write_to_reader(SmartCardChannelClient *scc)
{
    g_return_if_fail(scc);

    smartcard_channel_write_to_reader(scc->priv->write_buf);
    scc->priv->write_buf = NULL;
}


bool SmartCardChannelClient::handle_message(uint16_t type, uint32_t size, void *message)
{
    VSCMsgHeader* vheader = (VSCMsgHeader*) message;

    if (type != SPICE_MSGC_SMARTCARD_DATA) {
        /* Handles seamless migration protocol. Also handles ack's */
        return RedChannelClient::handle_message(type, size, message);
    }

    switch (vheader->type) {
        case VSC_ReaderAdd:
            smartcard_channel_client_add_reader(this);
            return TRUE;
            break;
        case VSC_ReaderRemove:
            smartcard_channel_client_remove_reader(this, vheader->reader_id);
            return TRUE;
            break;
        case VSC_Init:
            // ignore - we should never get this anyway
            return TRUE;
            break;
        case VSC_Error:
        case VSC_ATR:
        case VSC_CardRemove:
        case VSC_APDU:
            break; // passed on to device
        default:
            red_channel_warning(get_channel(),
                                "ERROR: unexpected message on smartcard channel");
            return TRUE;
    }

    /* todo: fix */
    if (vheader->reader_id >= smartcard_get_n_readers()) {
        red_channel_warning(get_channel(),
                            "ERROR: received message for non existing reader: %d, %d, %d",
                            vheader->reader_id, vheader->type, vheader->length);
        return FALSE;
    }
    spice_assert(priv->write_buf->buf_size >= size);
    memcpy(priv->write_buf->buf, message, size);
    smartcard_channel_client_write_to_reader(this);

    return TRUE;
}

bool SmartCardChannelClient::handle_migrate_data(uint32_t size, void *message)
{
    SmartCardChannelClient *scc = this;
    SpiceMigrateDataHeader *header;
    SpiceMigrateDataSmartcard *mig_data;

    header = (SpiceMigrateDataHeader *)message;
    mig_data = (SpiceMigrateDataSmartcard *)(header + 1);
    if (size < sizeof(SpiceMigrateDataHeader) + sizeof(SpiceMigrateDataSmartcard)) {
        spice_error("bad message size");
        return FALSE;
    }
    if (!migration_protocol_validate_header(header,
                                            SPICE_MIGRATE_DATA_SMARTCARD_MAGIC,
                                            SPICE_MIGRATE_DATA_SMARTCARD_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }

    if (!mig_data->base.connected) { /* client wasn't attached to a smartcard */
        return TRUE;
    }

    auto smartcard = scc->priv->smartcard.lock();
    if (!smartcard) {
        SpiceCharDeviceInstance *char_device = smartcard_readers_get_unattached();

        if (!char_device) {
            spice_warning("no unattached device available");
            return TRUE;
        } else {
            smartcard_char_device_attach_client(char_device, scc);
        }
        smartcard = scc->priv->smartcard.lock();
    }
    spice_debug("reader added %d partial read_size %u", mig_data->reader_added, mig_data->read_size);

    if (smartcard) {
        return smartcard_char_device_handle_migrate_data(smartcard.get(), mig_data);
    }
    return TRUE;
}

void SmartCardChannelClient::handle_migrate_flush_mark()
{
    pipe_add_type(RED_PIPE_ITEM_TYPE_SMARTCARD_MIGRATE_DATA);
}

void smartcard_channel_client_set_char_device(SmartCardChannelClient *scc,
                                              RedCharDeviceSmartcard *device)
{
    scc->priv->smartcard.reset(device);
}

red::shared_ptr<RedCharDeviceSmartcard>
smartcard_channel_client_get_char_device(SmartCardChannelClient *scc)
{
    return scc->priv->smartcard.lock();
}
