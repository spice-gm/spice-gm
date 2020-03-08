/* spice-server char device flow control code

   Copyright (C) 2012-2015 Red Hat, Inc.

   Red Hat Authors:
   Yonit Halperin <yhalperi@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http:www.gnu.org/licenses/>.
*/


#include <config.h>
#include <inttypes.h>

#include "char-device.h"
#include "reds.h"

#define CHAR_DEVICE_WRITE_TO_TIMEOUT 100
#define RED_CHAR_DEVICE_WAIT_TOKENS_TIMEOUT 30000

typedef enum {
    WRITE_BUFFER_ORIGIN_NONE,
    WRITE_BUFFER_ORIGIN_CLIENT,
    WRITE_BUFFER_ORIGIN_SERVER,
    WRITE_BUFFER_ORIGIN_SERVER_NO_TOKEN,
} WriteBufferOrigin;

struct RedCharDeviceWriteBufferPrivate {
    RedCharDeviceClientOpaque *client; /* The client that sent the message to the device.
                          NULL if the server created the message */
    WriteBufferOrigin origin;
    uint32_t token_price;
    uint32_t refs;
};

typedef struct RedCharDeviceClient RedCharDeviceClient;
struct RedCharDeviceClient {
    RedCharDevice *dev;
    RedCharDeviceClientOpaque *client;
    int do_flow_control;
    uint64_t num_client_tokens;
    uint64_t num_client_tokens_free; /* client messages that were consumed by the device */
    uint64_t num_send_tokens; /* send to client */
    SpiceTimer *wait_for_tokens_timer;
    int wait_for_tokens_started;
    GQueue *send_queue;
    uint32_t max_send_queue_size;
};

struct RedCharDevicePrivate {
    int running;
    int active; /* has read/write been performed since the device was started */
    int wait_for_migrate_data;

    GQueue write_queue;
    RedCharDeviceWriteBuffer *cur_write_buf;
    uint8_t *cur_write_buf_pos;
    SpiceTimer *write_to_dev_timer;
    uint64_t num_self_tokens;

    GList *clients; /* list of RedCharDeviceClient */

    uint64_t client_tokens_interval; /* frequency of returning tokens to the client */
    SpiceCharDeviceInstance *sin;

    int during_read_from_device;
    int during_write_to_device;

    SpiceServer *reds;
};

G_DEFINE_TYPE_WITH_PRIVATE(RedCharDevice, red_char_device, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_CHAR_DEV_INSTANCE,
    PROP_SPICE_SERVER,
    PROP_CLIENT_TOKENS_INTERVAL,
    PROP_SELF_TOKENS,
};

static void red_char_device_write_buffer_unref(RedCharDeviceWriteBuffer *write_buf);
static void red_char_device_init_device_instance(RedCharDevice *self);

static RedPipeItem *
red_char_device_read_one_msg_from_device(RedCharDevice *dev)
{
   RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(dev);

   return klass->read_one_msg_from_device(dev, dev->priv->sin);
}

static void
red_char_device_send_msg_to_client(RedCharDevice *dev,
                                   RedPipeItem *msg,
                                   RedCharDeviceClientOpaque *client)
{
    RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(dev);

    if (klass->send_msg_to_client != NULL) {
        klass->send_msg_to_client(dev, msg, client);
    }
}

static void
red_char_device_send_tokens_to_client(RedCharDevice *dev,
                                      RedCharDeviceClientOpaque *client,
                                      uint32_t tokens)
{
   RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(dev);

   if (klass->send_tokens_to_client == NULL) {
       g_warn_if_reached();
       return;
   }
   klass->send_tokens_to_client(dev, client, tokens);
}

static void
red_char_device_on_free_self_token(RedCharDevice *dev)
{
   RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(dev);

   if (klass->on_free_self_token != NULL) {
       klass->on_free_self_token(dev);
   }
}

static void
red_char_device_remove_client(RedCharDevice *dev, RedCharDeviceClientOpaque *client)
{
   RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(dev);

   klass->remove_client(dev, client);
}

static void red_char_device_write_buffer_free(RedCharDeviceWriteBuffer *buf)
{
    if (buf) {
        g_free(buf->priv);
    }
    /* NOTE: do not free buf. buf was contained into a larger structure
     * which contained both private and public part and was freed above */
}

static void write_buffers_queue_free(GQueue *write_queue)
{
    RedCharDeviceWriteBuffer *buf;
    while ((buf = (RedCharDeviceWriteBuffer *) g_queue_pop_tail(write_queue)))
        red_char_device_write_buffer_free(buf);
}

static void red_char_device_client_free(RedCharDevice *dev,
                                        RedCharDeviceClient *dev_client)
{
    GList *l, *next;

    red_timer_remove(dev_client->wait_for_tokens_timer);
    dev_client->wait_for_tokens_timer = NULL;

    g_queue_free_full(dev_client->send_queue, (GDestroyNotify)red_pipe_item_unref);

    /* remove write buffers that are associated with the client */
    spice_debug("write_queue_is_empty %d", g_queue_is_empty(&dev->priv->write_queue) && !dev->priv->cur_write_buf);
    l = g_queue_peek_head_link(&dev->priv->write_queue);
    while (l) {
        RedCharDeviceWriteBuffer *write_buf = (RedCharDeviceWriteBuffer *) l->data;
        next = l->next;

        if (write_buf->priv->origin == WRITE_BUFFER_ORIGIN_CLIENT &&
            write_buf->priv->client == dev_client->client) {
            g_queue_delete_link(&dev->priv->write_queue, l);
            red_char_device_write_buffer_unref(write_buf);
        }
        l = next;
    }

    if (dev->priv->cur_write_buf && dev->priv->cur_write_buf->priv->origin == WRITE_BUFFER_ORIGIN_CLIENT &&
        dev->priv->cur_write_buf->priv->client == dev_client->client) {
        dev->priv->cur_write_buf->priv->origin = WRITE_BUFFER_ORIGIN_NONE;
        dev->priv->cur_write_buf->priv->client = NULL;
    }

    dev->priv->clients = g_list_remove(dev->priv->clients, dev_client);
    g_free(dev_client);
}

static void red_char_device_handle_client_overflow(RedCharDeviceClient *dev_client)
{
    RedCharDevice *dev = dev_client->dev;
    red_char_device_remove_client(dev, dev_client->client);
}

static RedCharDeviceClient *red_char_device_client_find(RedCharDevice *dev,
                                                        RedCharDeviceClientOpaque *client)
{
    RedCharDeviceClient *dev_client;

    GLIST_FOREACH(dev->priv->clients, RedCharDeviceClient, dev_client) {
        if (dev_client->client == client) {
            return dev_client;
        }
    }
    return NULL;
}

/***************************
 * Reading from the device *
 **************************/

static void device_client_wait_for_tokens_timeout(RedCharDeviceClient *dev_client)
{
    red_char_device_handle_client_overflow(dev_client);
}

static int red_char_device_can_send_to_client(RedCharDeviceClient *dev_client)
{
    return !dev_client->do_flow_control || dev_client->num_send_tokens;
}

static uint64_t red_char_device_max_send_tokens(RedCharDevice *dev)
{
    RedCharDeviceClient *dev_client;
    uint64_t max = 0;

    GLIST_FOREACH(dev->priv->clients, RedCharDeviceClient, dev_client) {
        if (!dev_client->do_flow_control) {
            max = ~0;
            break;
        }

        if (dev_client->num_send_tokens > max) {
            max = dev_client->num_send_tokens;
        }
    }
    return max;
}

static void red_char_device_add_msg_to_client_queue(RedCharDeviceClient *dev_client,
                                                    RedPipeItem *msg)
{
    if (g_queue_get_length(dev_client->send_queue) >= dev_client->max_send_queue_size) {
        red_char_device_handle_client_overflow(dev_client);
        return;
    }

    red_pipe_item_ref(msg);
    g_queue_push_head(dev_client->send_queue, msg);
    if (!dev_client->wait_for_tokens_started) {
        red_timer_start(dev_client->wait_for_tokens_timer,
                        RED_CHAR_DEVICE_WAIT_TOKENS_TIMEOUT);
        dev_client->wait_for_tokens_started = TRUE;
    }
}

static void red_char_device_send_msg_to_clients(RedCharDevice *dev,
                                                RedPipeItem *msg)
{
    RedCharDeviceClient *dev_client;

    GLIST_FOREACH(dev->priv->clients, RedCharDeviceClient, dev_client) {
        if (red_char_device_can_send_to_client(dev_client)) {
            dev_client->num_send_tokens--;
            spice_assert(g_queue_is_empty(dev_client->send_queue));
            red_char_device_send_msg_to_client(dev, msg, dev_client->client);

            /* don't refer to dev_client anymore, it may have been released */
        } else {
            red_char_device_add_msg_to_client_queue(dev_client, msg);
        }
    }
}

static bool red_char_device_read_from_device(RedCharDevice *dev)
{
    uint64_t max_send_tokens;
    int did_read = FALSE;

    if (!dev->priv->running || dev->priv->wait_for_migrate_data || !dev->priv->sin) {
        return FALSE;
    }

    /* There are 2 scenarios where we can get called recursively:
     * 1) spice-vmc vmc_read triggering flush of throttled data, recalling wakeup
     * (virtio)
     * 2) in case of sending messages to the client, and unreferencing the
     * msg, we trigger another read.
     */
    if (dev->priv->during_read_from_device++ > 0) {
        return FALSE;
    }

    max_send_tokens = red_char_device_max_send_tokens(dev);
    dev->ref();
    /*
     * Reading from the device only in case at least one of the clients have a free token.
     * All messages will be discarded if no client is attached to the device
     */
    while ((max_send_tokens || (dev->priv->clients == NULL)) && dev->priv->running) {
        RedPipeItem *msg;

        msg = red_char_device_read_one_msg_from_device(dev);
        if (!msg) {
            if (dev->priv->during_read_from_device > 1) {
                dev->priv->during_read_from_device = 1;
                continue; /* a wakeup might have been called during the read -
                             make sure it doesn't get lost */
            }
            break;
        }
        did_read = TRUE;
        red_char_device_send_msg_to_clients(dev, msg);
        red_pipe_item_unref(msg);
        max_send_tokens--;
    }
    dev->priv->during_read_from_device = 0;
    if (dev->priv->running) {
        dev->priv->active = dev->priv->active || did_read;
    }
    dev->unref();
    return did_read;
}

static void red_char_device_client_send_queue_push(RedCharDeviceClient *dev_client)
{
    while (!g_queue_is_empty(dev_client->send_queue) &&
           red_char_device_can_send_to_client(dev_client)) {
        RedPipeItem *msg = (RedPipeItem *) g_queue_pop_tail(dev_client->send_queue);
        g_assert(msg != NULL);
        dev_client->num_send_tokens--;
        red_char_device_send_msg_to_client(dev_client->dev, msg,
                                           dev_client->client);
        red_pipe_item_unref(msg);
    }
}

static void
red_char_device_send_to_client_tokens_absorb(RedCharDevice *dev,
                                             RedCharDeviceClientOpaque *client,
                                             uint32_t tokens,
                                             bool reset)
{
    RedCharDeviceClient *dev_client;

    dev_client = red_char_device_client_find(dev, client);

    if (!dev_client) {
        spice_error("client wasn't found dev %p client %p", dev, client);
        return;
    }

    if (reset) {
        dev_client->num_send_tokens = 0;
    }
    dev_client->num_send_tokens += tokens;

    if (g_queue_get_length(dev_client->send_queue)) {
        spice_assert(dev_client->num_send_tokens == tokens);
        red_char_device_client_send_queue_push(dev_client);
    }

    if (red_char_device_can_send_to_client(dev_client)) {
        red_timer_cancel(dev_client->wait_for_tokens_timer);
        dev_client->wait_for_tokens_started = FALSE;
        red_char_device_read_from_device(dev_client->dev);
    } else if (!g_queue_is_empty(dev_client->send_queue)) {
        red_timer_start(dev_client->wait_for_tokens_timer,
                        RED_CHAR_DEVICE_WAIT_TOKENS_TIMEOUT);
        dev_client->wait_for_tokens_started = TRUE;
    }
}

void RedCharDevice::send_to_client_tokens_add(RedCharDeviceClientOpaque *client,
                                              uint32_t tokens)
{
    red_char_device_send_to_client_tokens_absorb(this, client, tokens, false);
}

void RedCharDevice::send_to_client_tokens_set(RedCharDeviceClientOpaque *client,
                                              uint32_t tokens)
{
    red_char_device_send_to_client_tokens_absorb(this, client, tokens, true);
}

/**************************
 * Writing to the device  *
***************************/

static void red_char_device_client_tokens_add(RedCharDevice *dev,
                                              RedCharDeviceClient *dev_client,
                                              uint32_t num_tokens)
{
    if (!dev_client->do_flow_control) {
        return;
    }
    if (num_tokens > 1) {
        spice_debug("#tokens > 1 (=%u)", num_tokens);
    }
    dev_client->num_client_tokens_free += num_tokens;
    if (dev_client->num_client_tokens_free >= dev->priv->client_tokens_interval) {
        uint32_t tokens = dev_client->num_client_tokens_free;

        dev_client->num_client_tokens += dev_client->num_client_tokens_free;
        dev_client->num_client_tokens_free = 0;
        red_char_device_send_tokens_to_client(dev, dev_client->client, tokens);
    }
}

int RedCharDevice::write_to_device()
{
    SpiceCharDeviceInterface *sif;
    int total = 0;
    int n;

    if (!priv->running || priv->wait_for_migrate_data || !priv->sin) {
        return 0;
    }

    /* protect against recursion with red_char_device_wakeup */
    if (priv->during_write_to_device++ > 0) {
        return 0;
    }

    ref();

    if (priv->write_to_dev_timer) {
        red_timer_cancel(priv->write_to_dev_timer);
    }

    sif = spice_char_device_get_interface(priv->sin);
    while (priv->running) {
        uint32_t write_len;

        if (!priv->cur_write_buf) {
            priv->cur_write_buf = (RedCharDeviceWriteBuffer *) g_queue_pop_tail(&priv->write_queue);
            if (!priv->cur_write_buf)
                break;
            priv->cur_write_buf_pos = priv->cur_write_buf->buf;
        }

        write_len = priv->cur_write_buf->buf + priv->cur_write_buf->buf_used -
                    priv->cur_write_buf_pos;
        n = sif->write(priv->sin, priv->cur_write_buf_pos, write_len);
        if (n <= 0) {
            if (priv->during_write_to_device > 1) {
                priv->during_write_to_device = 1;
                continue; /* a wakeup might have been called during the write -
                             make sure it doesn't get lost */
            }
            break;
        }
        total += n;
        write_len -= n;
        if (!write_len) {
            write_buffer_release(&priv->cur_write_buf);
            continue;
        }
        priv->cur_write_buf_pos += n;
    }
    /* retry writing as long as the write queue is not empty */
    if (priv->running) {
        if (priv->cur_write_buf) {
            if (priv->write_to_dev_timer) {
                red_timer_start(priv->write_to_dev_timer,
                                CHAR_DEVICE_WRITE_TO_TIMEOUT);
            }
        } else {
            spice_assert(g_queue_is_empty(&priv->write_queue));
        }
        priv->active = priv->active || total;
    }
    priv->during_write_to_device = 0;
    unref();
    return total;
}

void RedCharDevice::write_retry(RedCharDevice *dev)
{
    if (dev->priv->write_to_dev_timer) {
        red_timer_cancel(dev->priv->write_to_dev_timer);
    }
    dev->write_to_device();
}

static RedCharDeviceWriteBuffer *
red_char_device_write_buffer_get(RedCharDevice *dev, RedCharDeviceClientOpaque *client, int size,
                                 WriteBufferOrigin origin, int migrated_data_tokens)
{
    RedCharDeviceWriteBuffer *ret;

    if (origin == WRITE_BUFFER_ORIGIN_SERVER && !dev->priv->num_self_tokens) {
        return NULL;
    }

    struct RedCharDeviceWriteBufferFull {
        RedCharDeviceWriteBufferPrivate priv;
        RedCharDeviceWriteBuffer buffer;
    } *write_buf;
    write_buf = (struct RedCharDeviceWriteBufferFull* )
        g_malloc(sizeof(struct RedCharDeviceWriteBufferFull) + size);
    memset(write_buf, 0, sizeof(*write_buf));
    write_buf->priv.refs = 1;
    ret = &write_buf->buffer;
    ret->buf_size = size;
    ret->priv = &write_buf->priv;

    spice_assert(!ret->buf_used);

    ret->priv->origin = origin;

    if (origin == WRITE_BUFFER_ORIGIN_CLIENT) {
       spice_assert(client);
       RedCharDeviceClient *dev_client = red_char_device_client_find(dev, client);
       if (dev_client) {
            if (!migrated_data_tokens &&
                dev_client->do_flow_control && !dev_client->num_client_tokens) {
                g_warning("token violation: dev %p client %p", dev, client);
                red_char_device_handle_client_overflow(dev_client);
                goto error;
            }
            ret->priv->client = client;
            if (!migrated_data_tokens && dev_client->do_flow_control) {
                dev_client->num_client_tokens--;
            }
        } else {
            /* it is possible that the client was removed due to send tokens underflow, but
             * the caller still receive messages from the client */
            g_warning("client not found: dev %p client %p", dev, client);
            goto error;
        }
    } else if (origin == WRITE_BUFFER_ORIGIN_SERVER) {
        dev->priv->num_self_tokens--;
    }

    ret->priv->token_price = migrated_data_tokens ? migrated_data_tokens : 1;
    ret->priv->refs = 1;
    return ret;
error:
    red_char_device_write_buffer_free(ret);
    return NULL;
}

RedCharDeviceWriteBuffer *RedCharDevice::write_buffer_get_client(RedCharDeviceClientOpaque *client,
                                                                 int size)
{
    spice_assert(client);
    return  red_char_device_write_buffer_get(this, client, size, WRITE_BUFFER_ORIGIN_CLIENT, 0);
}

RedCharDeviceWriteBuffer *RedCharDevice::write_buffer_get_server(int size,
                                                                 bool use_token)
{
    WriteBufferOrigin origin =
        use_token ? WRITE_BUFFER_ORIGIN_SERVER : WRITE_BUFFER_ORIGIN_SERVER_NO_TOKEN;
    return  red_char_device_write_buffer_get(this, NULL, size, origin, 0);
}

static RedCharDeviceWriteBuffer *red_char_device_write_buffer_ref(RedCharDeviceWriteBuffer *write_buf)
{
    spice_assert(write_buf);

    write_buf->priv->refs++;
    return write_buf;
}

static void red_char_device_write_buffer_unref(RedCharDeviceWriteBuffer *write_buf)
{
    spice_assert(write_buf);

    write_buf->priv->refs--;
    if (write_buf->priv->refs == 0)
        red_char_device_write_buffer_free(write_buf);
}

void RedCharDevice::write_buffer_add(RedCharDeviceWriteBuffer *write_buf)
{

    /* caller shouldn't add buffers for client that was removed */
    if (write_buf->priv->origin == WRITE_BUFFER_ORIGIN_CLIENT &&
        !red_char_device_client_find(this, write_buf->priv->client)) {
        g_warning("client not found: this %p client %p", this, write_buf->priv->client);
        red_char_device_write_buffer_unref(write_buf);
        return;
    }

    g_queue_push_head(&priv->write_queue, write_buf);
    write_to_device();
}

void RedCharDevice::write_buffer_release(RedCharDevice *dev,
                                         RedCharDeviceWriteBuffer **p_write_buf)
{
    RedCharDeviceWriteBuffer *write_buf = *p_write_buf;
    if (!write_buf) {
        return;
    }
    *p_write_buf = NULL;

    WriteBufferOrigin buf_origin = write_buf->priv->origin;
    uint32_t buf_token_price = write_buf->priv->token_price;
    RedCharDeviceClientOpaque *client = write_buf->priv->client;

    if (!dev) {
        g_warning("no device. write buffer is freed");
        red_char_device_write_buffer_free(write_buf);
        return;
    }

    spice_assert(dev->priv->cur_write_buf != write_buf);

    red_char_device_write_buffer_unref(write_buf);
    if (buf_origin == WRITE_BUFFER_ORIGIN_CLIENT) {
        RedCharDeviceClient *dev_client;

        spice_assert(client);
        dev_client = red_char_device_client_find(dev, client);
        /* when a client is removed, we remove all the buffers that are associated with it */
        spice_assert(dev_client);
        red_char_device_client_tokens_add(dev, dev_client, buf_token_price);
    } else if (buf_origin == WRITE_BUFFER_ORIGIN_SERVER) {
        dev->priv->num_self_tokens++;
        red_char_device_on_free_self_token(dev);
    }
}

/********************************
 * char_device_state management *
 ********************************/

void RedCharDevice::reset_dev_instance(SpiceCharDeviceInstance *sin)
{
    spice_debug("sin %p, char device %p", sin, this);
    priv->sin = sin;
    if (sin)
        sin->st = this;
    if (priv->reds) {
        red_char_device_init_device_instance(this);
    }
}

static RedCharDeviceClient *
red_char_device_client_new(RedsState *reds,
                           RedCharDeviceClientOpaque *client,
                           int do_flow_control,
                           uint32_t max_send_queue_size,
                           uint32_t num_client_tokens,
                           uint32_t num_send_tokens)
{
    RedCharDeviceClient *dev_client;

    dev_client = g_new0(RedCharDeviceClient, 1);
    dev_client->client = client;
    dev_client->send_queue = g_queue_new();
    dev_client->max_send_queue_size = max_send_queue_size;
    dev_client->do_flow_control = do_flow_control;
    if (do_flow_control) {
        dev_client->wait_for_tokens_timer =
            reds_core_timer_add(reds, device_client_wait_for_tokens_timeout,
                                dev_client);
        if (!dev_client->wait_for_tokens_timer) {
            spice_error("failed to create wait for tokens timer");
        }
        dev_client->num_client_tokens = num_client_tokens;
        dev_client->num_send_tokens = num_send_tokens;
    } else {
        dev_client->num_client_tokens = ~0;
        dev_client->num_send_tokens = ~0;
    }

    return dev_client;
}

bool RedCharDevice::client_add(RedCharDeviceClientOpaque *client,
                               int do_flow_control,
                               uint32_t max_send_queue_size,
                               uint32_t num_client_tokens,
                               uint32_t num_send_tokens,
                               int wait_for_migrate_data)
{
    RedCharDeviceClient *dev_client;


    spice_assert(client);

    if (wait_for_migrate_data && (priv->clients != NULL || priv->active)) {
        spice_warning("can't restore device %p from migration data. The device "
                      "has already been active", this);
        return FALSE;
    }

    priv->wait_for_migrate_data = wait_for_migrate_data;

    spice_debug("char device %p, client %p", this, client);
    dev_client = red_char_device_client_new(priv->reds,
                                            client,
                                            do_flow_control,
                                            max_send_queue_size,
                                            num_client_tokens,
                                            num_send_tokens);
    dev_client->dev = this;
    priv->clients = g_list_prepend(priv->clients, dev_client);
    /* Now that we have a client, forward any pending device data */
    wakeup();
    return TRUE;
}

void RedCharDevice::client_remove(RedCharDeviceClientOpaque *client)
{
    RedCharDeviceClient *dev_client;

    spice_debug("char device %p, client %p", this, client);
    dev_client = red_char_device_client_find(this, client);

    if (!dev_client) {
        spice_error("client wasn't found");
        return;
    }
    red_char_device_client_free(this, dev_client);
    if (priv->wait_for_migrate_data) {
        spice_assert(priv->clients == NULL);
        priv->wait_for_migrate_data  = FALSE;
        red_char_device_read_from_device(this);
    }
}

int RedCharDevice::client_exists(RedCharDeviceClientOpaque *client)
{
    return (red_char_device_client_find(this, client) != NULL);
}

void RedCharDevice::start()
{
    spice_debug("char device %p", this);
    priv->running = TRUE;
    ref();
    while (write_to_device() ||
           red_char_device_read_from_device(this));
    unref();
}

void RedCharDevice::stop()
{
    spice_debug("char device %p", this);
    priv->running = FALSE;
    priv->active = FALSE;
    if (priv->write_to_dev_timer) {
        red_timer_cancel(priv->write_to_dev_timer);
    }
}

void RedCharDevice::reset()
{
    RedCharDeviceClient *dev_client;
    RedCharDeviceWriteBuffer *buf;

    priv->wait_for_migrate_data = FALSE;
    spice_debug("char device %p", this);
    while ((buf = (RedCharDeviceWriteBuffer *) g_queue_pop_tail(&priv->write_queue))) {
        write_buffer_release(&buf);
    }
    write_buffer_release(&priv->cur_write_buf);

    GLIST_FOREACH(priv->clients, RedCharDeviceClient, dev_client) {
        spice_debug("send_queue_empty %d", g_queue_is_empty(dev_client->send_queue));
        dev_client->num_send_tokens += g_queue_get_length(dev_client->send_queue);
        g_queue_free_full(dev_client->send_queue, (GDestroyNotify)red_pipe_item_unref);
        dev_client->send_queue = g_queue_new();

        /* If device is reset, we must reset the tokens counters as well as we
         * don't hold any data from client and upon agent's reconnection we send
         * SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS with all free tokens we have */
        dev_client->num_client_tokens += dev_client->num_client_tokens_free;
        dev_client->num_client_tokens_free = 0;
    }
}

void RedCharDevice::wakeup()
{
    write_to_device();
    red_char_device_read_from_device(this);
}

/*************
 * Migration *
 * **********/

void RedCharDevice::migrate_data_marshall_empty(SpiceMarshaller *m)
{
    SpiceMigrateDataCharDevice *mig_data;

    spice_debug("trace");
    mig_data = (SpiceMigrateDataCharDevice *)spice_marshaller_reserve_space(m,
                                                                            sizeof(*mig_data));
    memset(mig_data, 0, sizeof(*mig_data));
    mig_data->version = SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION;
    mig_data->connected = FALSE;
}

static void migrate_data_marshaller_write_buffer_free(uint8_t *data, void *opaque)
{
    RedCharDeviceWriteBuffer *write_buf = (RedCharDeviceWriteBuffer *)opaque;

    red_char_device_write_buffer_unref(write_buf);
}

void RedCharDevice::migrate_data_marshall(SpiceMarshaller *m)
{
    RedCharDeviceClient *dev_client;
    GList *item;
    uint8_t *write_to_dev_sizes_ptr;
    uint32_t write_to_dev_size;
    uint32_t write_to_dev_tokens;
    SpiceMarshaller *m2;

    /* multi-clients are not supported */
    spice_assert(g_list_length(priv->clients) == 1);
    dev_client = (RedCharDeviceClient *) g_list_last(priv->clients)->data;
    /* FIXME: if there were more than one client before the marshalling,
     * it is possible that the send_queue length > 0, and the send data
     * should be migrated as well */
    spice_assert(g_queue_is_empty(dev_client->send_queue));
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION);
    spice_marshaller_add_uint8(m, 1); /* connected */
    spice_marshaller_add_uint32(m, dev_client->num_client_tokens);
    spice_marshaller_add_uint32(m, dev_client->num_send_tokens);
    write_to_dev_sizes_ptr = spice_marshaller_reserve_space(m, sizeof(uint32_t)*2);
    write_to_dev_size = 0;
    write_to_dev_tokens = 0;

    m2 = spice_marshaller_get_ptr_submarshaller(m);
    if (priv->cur_write_buf) {
        uint32_t buf_remaining = priv->cur_write_buf->buf + priv->cur_write_buf->buf_used -
                                 priv->cur_write_buf_pos;
        spice_marshaller_add_by_ref_full(m2, priv->cur_write_buf_pos, buf_remaining,
                                         migrate_data_marshaller_write_buffer_free,
                                         red_char_device_write_buffer_ref(priv->cur_write_buf)
                                         );
        write_to_dev_size += buf_remaining;
        if (priv->cur_write_buf->priv->origin == WRITE_BUFFER_ORIGIN_CLIENT) {
            spice_assert(priv->cur_write_buf->priv->client == dev_client->client);
            write_to_dev_tokens += priv->cur_write_buf->priv->token_price;
        }
    }

    for (item = g_queue_peek_tail_link(&priv->write_queue); item != NULL; item = item->prev) {
        RedCharDeviceWriteBuffer *write_buf = (RedCharDeviceWriteBuffer *) item->data;

        spice_marshaller_add_by_ref_full(m2, write_buf->buf, write_buf->buf_used,
                                         migrate_data_marshaller_write_buffer_free,
                                         red_char_device_write_buffer_ref(write_buf)
                                         );
        write_to_dev_size += write_buf->buf_used;
        if (write_buf->priv->origin == WRITE_BUFFER_ORIGIN_CLIENT) {
            spice_assert(write_buf->priv->client == dev_client->client);
            write_to_dev_tokens += write_buf->priv->token_price;
        }
    }
    spice_debug("migration data dev %p: write_queue size %u tokens %u",
                this, write_to_dev_size, write_to_dev_tokens);
    spice_marshaller_set_uint32(m, write_to_dev_sizes_ptr, write_to_dev_size);
    spice_marshaller_set_uint32(m, write_to_dev_sizes_ptr + sizeof(uint32_t), write_to_dev_tokens);
}

bool RedCharDevice::restore(SpiceMigrateDataCharDevice *mig_data)
{
    RedCharDeviceClient *dev_client;
    uint32_t client_tokens_window;

    spice_assert(g_list_length(priv->clients) == 1 &&
                 priv->wait_for_migrate_data);

    dev_client = (RedCharDeviceClient *) g_list_last(priv->clients)->data;
    if (mig_data->version > SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION) {
        spice_error("dev %p error: migration data version %u is bigger than self %u",
                    this, mig_data->version, SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION);
        return FALSE;
    }
    spice_assert(!priv->cur_write_buf && g_queue_is_empty(&priv->write_queue));
    spice_assert(mig_data->connected);

    client_tokens_window = dev_client->num_client_tokens; /* initial state of tokens */
    dev_client->num_client_tokens = mig_data->num_client_tokens;
    /* assumption: client_tokens_window stays the same across severs */
    dev_client->num_client_tokens_free = client_tokens_window -
                                           mig_data->num_client_tokens -
                                           mig_data->write_num_client_tokens;
    dev_client->num_send_tokens = mig_data->num_send_tokens;

    if (mig_data->write_size > 0) {
        if (mig_data->write_num_client_tokens) {
            priv->cur_write_buf =
                red_char_device_write_buffer_get(this, dev_client->client,
                    mig_data->write_size, WRITE_BUFFER_ORIGIN_CLIENT,
                    mig_data->write_num_client_tokens);
        } else {
            priv->cur_write_buf =
                red_char_device_write_buffer_get(this, NULL,
                    mig_data->write_size, WRITE_BUFFER_ORIGIN_SERVER, 0);
        }
        /* the first write buffer contains all the data that was saved for migration */
        memcpy(priv->cur_write_buf->buf,
               ((uint8_t *)mig_data) + mig_data->write_data_ptr - sizeof(SpiceMigrateDataHeader),
               mig_data->write_size);
        priv->cur_write_buf->buf_used = mig_data->write_size;
        priv->cur_write_buf_pos = priv->cur_write_buf->buf;
    }
    priv->wait_for_migrate_data = FALSE;
    write_to_device();
    red_char_device_read_from_device(this);
    return TRUE;
}

SpiceServer* RedCharDevice::get_server()
{
    return priv->reds;
}

SpiceCharDeviceInterface *spice_char_device_get_interface(SpiceCharDeviceInstance *instance)
{
   return SPICE_UPCAST(SpiceCharDeviceInterface, instance->base.sif);
}


static void red_char_device_init_device_instance(RedCharDevice *self)
{
    SpiceCharDeviceInterface *sif;

    g_return_if_fail(self->priv->reds);

    red_timer_remove(self->priv->write_to_dev_timer);
    self->priv->write_to_dev_timer = NULL;

    if (self->priv->sin == NULL) {
       return;
    }

    sif = spice_char_device_get_interface(self->priv->sin);
    if (sif->base.minor_version <= 2 ||
        !(sif->flags & SPICE_CHAR_DEVICE_NOTIFY_WRITABLE)) {
        self->priv->write_to_dev_timer = reds_core_timer_add(self->priv->reds,
                                                             RedCharDevice::write_retry,
                                                             self);
        if (!self->priv->write_to_dev_timer) {
            spice_error("failed creating char dev write timer");
        }
    }

    self->priv->sin->st = self;
}

static void
red_char_device_get_property(GObject    *object,
                             guint       property_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
    RedCharDevice *self = RED_CHAR_DEVICE(object);

    switch (property_id)
    {
        case PROP_CHAR_DEV_INSTANCE:
            g_value_set_pointer(value, self->priv->sin);
            break;
        case PROP_SPICE_SERVER:
            g_value_set_pointer(value, self->priv->reds);
            break;
        case PROP_CLIENT_TOKENS_INTERVAL:
            g_value_set_uint64(value, self->priv->client_tokens_interval);
            break;
        case PROP_SELF_TOKENS:
            g_value_set_uint64(value, self->priv->num_self_tokens);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
red_char_device_set_property(GObject      *object,
                             guint         property_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
    RedCharDevice *self = RED_CHAR_DEVICE(object);

    switch (property_id)
    {
        case PROP_CHAR_DEV_INSTANCE:
            self->reset_dev_instance((SpiceCharDeviceInstance *)g_value_get_pointer(value));
            break;
        case PROP_SPICE_SERVER:
            self->priv->reds = (SpiceServer *) g_value_get_pointer(value);
            if (self->priv->sin) {
                red_char_device_init_device_instance(self);
            }
            break;
        case PROP_CLIENT_TOKENS_INTERVAL:
            self->priv->client_tokens_interval = g_value_get_uint64(value);
            break;
        case PROP_SELF_TOKENS:
            self->priv->num_self_tokens = g_value_get_uint64(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
red_char_device_finalize(GObject *object)
{
    RedCharDevice *self = RED_CHAR_DEVICE(object);

    red_timer_remove(self->priv->write_to_dev_timer);
    self->priv->write_to_dev_timer = NULL;

    write_buffers_queue_free(&self->priv->write_queue);
    red_char_device_write_buffer_free(self->priv->cur_write_buf);
    self->priv->cur_write_buf = NULL;

    while (self->priv->clients != NULL) {
        RedCharDeviceClient *dev_client = (RedCharDeviceClient *) self->priv->clients->data;
        red_char_device_client_free(self, dev_client);
    }
    self->priv->running = FALSE;

    G_OBJECT_CLASS(red_char_device_parent_class)->finalize(object);
}

static void
port_event_none(RedCharDevice *self G_GNUC_UNUSED, uint8_t event G_GNUC_UNUSED)
{
}

static void
red_char_device_class_init(RedCharDeviceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = red_char_device_get_property;
    object_class->set_property = red_char_device_set_property;
    object_class->finalize = red_char_device_finalize;

    g_object_class_install_property(object_class,
                                    PROP_CHAR_DEV_INSTANCE,
                                    g_param_spec_pointer("sin",
                                                         "sin",
                                                         "Char device instance",
                                                         G_PARAM_STATIC_STRINGS |
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT));
    g_object_class_install_property(object_class,
                                    PROP_SPICE_SERVER,
                                    g_param_spec_pointer("spice-server",
                                                         "spice-server",
                                                         "RedsState instance",
                                                         G_PARAM_STATIC_STRINGS |
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT));
    g_object_class_install_property(object_class,
                                    PROP_CLIENT_TOKENS_INTERVAL,
                                    g_param_spec_uint64("client-tokens-interval",
                                                        "client-tokens-interval",
                                                        "Client token interval",
                                                        0, G_MAXUINT64, 0,
                                                        G_PARAM_STATIC_STRINGS |
                                                        G_PARAM_READWRITE));
    g_object_class_install_property(object_class,
                                    PROP_SELF_TOKENS,
                                    g_param_spec_uint64("self-tokens",
                                                        "self-tokens",
                                                        "Self tokens",
                                                        0, G_MAXUINT64, 0,
                                                        G_PARAM_STATIC_STRINGS |
                                                        G_PARAM_READWRITE));

    klass->port_event = port_event_none;
}

SPICE_GNUC_VISIBLE void spice_server_port_event(SpiceCharDeviceInstance *sin, uint8_t event)
{
    if (sin->st == NULL) {
        spice_warning("no RedCharDevice attached to instance %p", sin);
        return;
    }

    RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(sin->st);
    if (!klass) {
        // wrong object, a warning is already produced by RED_CHAR_DEVICE_GET_CLASS
        return;
    }

    return klass->port_event(sin->st, event);
}

SpiceCharDeviceInstance *RedCharDevice::get_device_instance()
{
    return priv->sin;
}

static void
red_char_device_init(RedCharDevice *self)
{
    self->priv = (RedCharDevicePrivate*) red_char_device_get_instance_private(self);

    g_queue_init(&self->priv->write_queue);
}
