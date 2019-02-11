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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "smartcard-channel-client.h"

struct SmartCardChannelClientPrivate
{
    RedCharDeviceSmartcard *smartcard;

    /* read_from_client/write_to_device buffer.
     * The beginning of the buffer should always be VSCMsgHeader*/
    RedCharDeviceWriteBuffer *write_buf;
    int msg_in_write_buf; /* was the client msg received into a RedCharDeviceWriteBuffer
                           * or was it explicitly malloced */
};

G_DEFINE_TYPE_WITH_PRIVATE(SmartCardChannelClient, smart_card_channel_client,
                           RED_TYPE_CHANNEL_CLIENT)

typedef struct RedErrorItem {
    RedPipeItem base;
    VSCMsgHeader vheader;
    VSCMsgError  error;
} RedErrorItem;

static uint8_t *
smartcard_channel_client_alloc_msg_rcv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size);
static void
smartcard_channel_client_release_msg_rcv_buf(RedChannelClient *rcc, uint16_t type,
                                             uint32_t size, uint8_t *msg);
static void smartcard_channel_client_on_disconnect(RedChannelClient *rcc);

static void smart_card_channel_client_get_property(GObject *object,
                                                   guint property_id,
                                                   GValue *value,
                                                   GParamSpec *pspec)
{
    switch (property_id)
    {
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void smart_card_channel_client_set_property(GObject *object,
                                                   guint property_id,
                                                   const GValue *value,
                                                   GParamSpec *pspec)
{
    switch (property_id)
    {
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void smart_card_channel_client_finalize(GObject *object)
{
    SmartCardChannelClient *self = SMARTCARD_CHANNEL_CLIENT(object);

    if (self->priv->smartcard)
        g_object_remove_weak_pointer(G_OBJECT(self->priv->smartcard),
                                     (gpointer*)&self->priv->smartcard);
    G_OBJECT_CLASS(smart_card_channel_client_parent_class)->finalize(object);
}

static void smart_card_channel_client_class_init(SmartCardChannelClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    RedChannelClientClass *client_class = RED_CHANNEL_CLIENT_CLASS(klass);
    client_class->alloc_recv_buf = smartcard_channel_client_alloc_msg_rcv_buf;
    client_class->release_recv_buf = smartcard_channel_client_release_msg_rcv_buf;
    client_class->on_disconnect = smartcard_channel_client_on_disconnect;

    object_class->get_property = smart_card_channel_client_get_property;
    object_class->set_property = smart_card_channel_client_set_property;
    object_class->finalize = smart_card_channel_client_finalize;
}

static void
smart_card_channel_client_init(SmartCardChannelClient *self)
{
    self->priv = smart_card_channel_client_get_instance_private(self);
}

SmartCardChannelClient* smartcard_channel_client_create(RedChannel *channel,
                                                        RedClient *client, RedStream *stream,
                                                        RedChannelCapabilities *caps)
{
    SmartCardChannelClient *rcc;

    rcc = g_initable_new(TYPE_SMARTCARD_CHANNEL_CLIENT,
                         NULL, NULL,
                         "channel", channel,
                         "client", client,
                         "stream", stream,
                         "caps", caps,
                         NULL);

    return rcc;
}

static uint8_t *
smartcard_channel_client_alloc_msg_rcv_buf(RedChannelClient *rcc,
                                           uint16_t type, uint32_t size)
{
    SmartCardChannelClient *scc = SMARTCARD_CHANNEL_CLIENT(rcc);
    RedClient *client = red_channel_client_get_client(rcc);

    /* todo: only one reader is actually supported. When we fix the code to support
     * multiple readers, we will porbably associate different devices to
     * differenc channels */
    if (!scc->priv->smartcard) {
        scc->priv->msg_in_write_buf = FALSE;
        return g_malloc(size);
    } else {
        RedCharDeviceSmartcard *smartcard;

        spice_assert(smartcard_get_n_readers() == 1);
        smartcard = scc->priv->smartcard;
        spice_assert(smartcard_char_device_get_client(smartcard) || scc->priv->smartcard);
        spice_assert(!scc->priv->write_buf);
        scc->priv->write_buf =
            red_char_device_write_buffer_get_client(RED_CHAR_DEVICE(smartcard), client, size);

        if (!scc->priv->write_buf) {
            spice_error("failed to allocate write buffer");
            return NULL;
        }
        scc->priv->msg_in_write_buf = TRUE;
        return scc->priv->write_buf->buf;
    }
}

static void
smartcard_channel_client_release_msg_rcv_buf(RedChannelClient *rcc,
                                             uint16_t type, uint32_t size, uint8_t *msg)
{
    SmartCardChannelClient *scc = SMARTCARD_CHANNEL_CLIENT(rcc);

    /* todo: only one reader is actually supported. When we fix the code to support
     * multiple readers, we will porbably associate different devices to
     * differenc channels */

    if (!scc->priv->msg_in_write_buf) {
        spice_assert(!scc->priv->write_buf);
        g_free(msg);
    } else {
        if (scc->priv->write_buf) { /* msg hasn't been pushed to the guest */
            spice_assert(scc->priv->write_buf->buf == msg);
            red_char_device_write_buffer_release(RED_CHAR_DEVICE(scc->priv->smartcard),
                                                 &scc->priv->write_buf);
        }
    }
}

static void smartcard_channel_client_on_disconnect(RedChannelClient *rcc)
{
    SmartCardChannelClient *scc = SMARTCARD_CHANNEL_CLIENT(rcc);
    RedCharDeviceSmartcard *device = scc->priv->smartcard;

    if (device) {
        smartcard_char_device_detach_client(device, scc);
        smartcard_char_device_notify_reader_remove(device);
    }
}

void smartcard_channel_client_send_data(RedChannelClient *rcc,
                                        SpiceMarshaller *m,
                                        RedPipeItem *item,
                                        VSCMsgHeader *vheader)
{
    spice_assert(rcc);
    spice_assert(vheader);
    /* NOTE: 'vheader' is assumed to be owned by 'item' so we keep the pipe
     * item valid until the message is actually sent. */
    red_pipe_item_ref(item);
    red_channel_client_init_send_data(rcc, SPICE_MSG_SMARTCARD_DATA);
    spice_marshaller_add_by_ref_full(m, (uint8_t*)vheader, sizeof(VSCMsgHeader) + vheader->length,
                                     marshaller_unref_pipe_item, item);
}

void smartcard_channel_client_send_error(RedChannelClient *rcc, SpiceMarshaller *m, RedPipeItem *item)
{
    RedErrorItem* error_item = SPICE_UPCAST(RedErrorItem, item);

    smartcard_channel_client_send_data(rcc, m, item, &error_item->vheader);
}

static void smartcard_channel_client_push_error(RedChannelClient *rcc,
                                                uint32_t reader_id,
                                                VSCErrorCode error)
{
    RedErrorItem *error_item = g_new0(RedErrorItem, 1);

    red_pipe_item_init(&error_item->base, RED_PIPE_ITEM_TYPE_ERROR);

    error_item->vheader.reader_id = reader_id;
    error_item->vheader.type = VSC_Error;
    error_item->vheader.length = sizeof(error_item->error);
    error_item->error.code = error;
    red_channel_client_pipe_add_push(rcc, &error_item->base);
}

static void smartcard_channel_client_add_reader(SmartCardChannelClient *scc,
                                                uint8_t *name)
{
    if (!scc->priv->smartcard) { /* we already tried to attach a reader to the client
                                          when it connected */
        SpiceCharDeviceInstance *char_device = smartcard_readers_get_unattached();

        if (!char_device) {
            smartcard_channel_client_push_error(RED_CHANNEL_CLIENT(scc),
                                                VSCARD_UNDEFINED_READER_ID,
                                                VSC_CANNOT_ADD_MORE_READERS);
            return;
        }
        smartcard_char_device_attach_client(char_device, scc);
    }
    smartcard_char_device_notify_reader_add(scc->priv->smartcard);
    // The device sends a VSC_Error message, we will let it through, no
    // need to send our own. We already set the correct reader_id, from
    // our RedCharDeviceSmartcard.
}

static void smartcard_channel_client_remove_reader(SmartCardChannelClient *scc,
                                                   uint32_t reader_id)
{
    SpiceCharDeviceInstance *char_device = smartcard_readers_get(reader_id);
    RedCharDeviceSmartcard *dev;

    if (char_device == NULL) {
        smartcard_channel_client_push_error(RED_CHANNEL_CLIENT(scc),
                                            reader_id, VSC_GENERAL_ERROR);
        return;
    }

    dev = RED_CHAR_DEVICE_SMARTCARD(char_device->st);
    spice_assert(scc->priv->smartcard == dev);
    if (!smartcard_char_device_notify_reader_remove(dev)) {
        smartcard_channel_client_push_error(RED_CHANNEL_CLIENT(scc),
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


bool smartcard_channel_client_handle_message(RedChannelClient *rcc,
                                             uint16_t type,
                                             uint32_t size,
                                             void *message)
{
    uint8_t *msg = message;
    VSCMsgHeader* vheader = message;
    SmartCardChannelClient *scc = SMARTCARD_CHANNEL_CLIENT(rcc);

    if (type != SPICE_MSGC_SMARTCARD_DATA) {
        /* Handles seamless migration protocol. Also handles ack's,
         * spicy sends them while spicec does not */
        return red_channel_client_handle_message(rcc, type, size, msg);
    }

    spice_assert(size == vheader->length + sizeof(VSCMsgHeader));
    switch (vheader->type) {
        case VSC_ReaderAdd:
            smartcard_channel_client_add_reader(scc, msg + sizeof(VSCMsgHeader));
            return TRUE;
            break;
        case VSC_ReaderRemove:
            smartcard_channel_client_remove_reader(scc, vheader->reader_id);
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
            printf("ERROR: unexpected message on smartcard channel\n");
            return TRUE;
    }

    /* todo: fix */
    if (vheader->reader_id >= smartcard_get_n_readers()) {
        red_channel_warning(red_channel_client_get_channel(rcc),
                            "ERROR: received message for non existing reader: %d, %d, %d",
                            vheader->reader_id, vheader->type, vheader->length);
        return FALSE;
    }
    spice_assert(scc->priv->write_buf->buf == msg);
    smartcard_channel_client_write_to_reader(scc);

    return TRUE;
}

bool smartcard_channel_client_handle_migrate_data(RedChannelClient *rcc,
                                                  uint32_t size,
                                                  void *message)
{
    SmartCardChannelClient *scc;
    SpiceMigrateDataHeader *header;
    SpiceMigrateDataSmartcard *mig_data;

    scc = SMARTCARD_CHANNEL_CLIENT(rcc);
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

    if (!scc->priv->smartcard) {
        SpiceCharDeviceInstance *char_device = smartcard_readers_get_unattached();

        if (!char_device) {
            spice_warning("no unattached device available");
            return TRUE;
        } else {
            smartcard_char_device_attach_client(char_device, scc);
        }
    }
    spice_debug("reader added %d partial read_size %u", mig_data->reader_added, mig_data->read_size);

    return smartcard_char_device_handle_migrate_data(scc->priv->smartcard,
                                                     mig_data);
}

bool smartcard_channel_client_handle_migrate_flush_mark(RedChannelClient *rcc)
{
    red_channel_client_pipe_add_type(rcc, RED_PIPE_ITEM_TYPE_SMARTCARD_MIGRATE_DATA);
    return TRUE;
}

void smartcard_channel_client_set_char_device(SmartCardChannelClient *scc,
                                              RedCharDeviceSmartcard *device)
{
    if (device == scc->priv->smartcard) {
        return;
    }

    if (scc->priv->smartcard) {
        g_object_remove_weak_pointer(G_OBJECT(scc->priv->smartcard),
                                     (gpointer*)&scc->priv->smartcard);
    }

    scc->priv->smartcard = device;
    if (scc->priv->smartcard) {
        g_object_add_weak_pointer(G_OBJECT(scc->priv->smartcard),
                                  (gpointer*)&scc->priv->smartcard);
    }
}

RedCharDeviceSmartcard* smartcard_channel_client_get_char_device(SmartCardChannelClient *scc)
{
    return scc->priv->smartcard;
}
