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
#include <list>

#include "char-device.h"
#include "reds.h"
#include "safe-list.hpp"

#define CHAR_DEVICE_WRITE_TO_TIMEOUT 100
#define RED_CHAR_DEVICE_WAIT_TOKENS_TIMEOUT 30000

enum WriteBufferOrigin {
    WRITE_BUFFER_ORIGIN_NONE,
    WRITE_BUFFER_ORIGIN_CLIENT,
    WRITE_BUFFER_ORIGIN_SERVER,
    WRITE_BUFFER_ORIGIN_SERVER_NO_TOKEN,
};

struct RedCharDeviceWriteBufferPrivate {
    RedCharDeviceClientOpaque *client; /* The client that sent the message to the device.
                          NULL if the server created the message */
    WriteBufferOrigin origin;
    uint32_t token_price;
    uint32_t refs;
};

struct RedCharDeviceClient {
    SPICE_CXX_GLIB_ALLOCATOR
    using Queue = std::list<RedPipeItemPtr, red::Mallocator<RedPipeItemPtr> >;

    RedCharDeviceClient(RedCharDevice *dev,
                        RedsState *reds,
                        RedCharDeviceClientOpaque *client,
                        bool do_flow_control,
                        uint32_t max_send_queue_size,
                        uint32_t num_client_tokens,
                        uint32_t num_send_tokens);
    ~RedCharDeviceClient();

    RedCharDevice *const dev;
    RedCharDeviceClientOpaque *const client;
    const bool do_flow_control;
    uint64_t num_client_tokens;
    uint64_t num_client_tokens_free; /* client messages that were consumed by the device */
    uint64_t num_send_tokens; /* send to client */
    SpiceTimer *wait_for_tokens_timer;
    int wait_for_tokens_started;
    Queue send_queue;
    const uint32_t max_send_queue_size;
};

struct RedCharDevicePrivate {
    SPICE_CXX_GLIB_ALLOCATOR

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

static void red_char_device_write_buffer_unref(RedCharDeviceWriteBuffer *write_buf);

void
RedCharDevice::send_tokens_to_client(RedCharDeviceClientOpaque *client, uint32_t tokens)
{
    g_warn_if_reached();
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
    dev_client->wait_for_tokens_timer = nullptr;

    dev_client->send_queue.clear();

    /* remove write buffers that are associated with the client */
    spice_debug("write_queue_is_empty %d", g_queue_is_empty(&dev->priv->write_queue) && !dev->priv->cur_write_buf);
    l = g_queue_peek_head_link(&dev->priv->write_queue);
    while (l) {
        auto write_buf = (RedCharDeviceWriteBuffer *) l->data;
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
        dev->priv->cur_write_buf->priv->client = nullptr;
    }

    dev->priv->clients = g_list_remove(dev->priv->clients, dev_client);
    delete dev_client;
}

static void red_char_device_handle_client_overflow(RedCharDeviceClient *dev_client)
{
    RedCharDevice *dev = dev_client->dev;
    dev->remove_client(dev_client->client);
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
    return nullptr;
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
    if (dev_client->send_queue.size() >= dev_client->max_send_queue_size) {
        red_char_device_handle_client_overflow(dev_client);
        return;
    }

    dev_client->send_queue.push_front(RedPipeItemPtr(msg));
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
            spice_assert(dev_client->send_queue.empty());
            dev->send_msg_to_client(msg, dev_client->client);

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
    red::shared_ptr<RedCharDevice> hold_dev(dev);
    /*
     * Reading from the device only in case at least one of the clients have a free token.
     * All messages will be discarded if no client is attached to the device
     */
    while ((max_send_tokens || (dev->priv->clients == nullptr)) && dev->priv->running) {
        auto msg = dev->read_one_msg_from_device();
        if (!msg) {
            if (dev->priv->during_read_from_device > 1) {
                dev->priv->during_read_from_device = 1;
                continue; /* a wakeup might have been called during the read -
                             make sure it doesn't get lost */
            }
            break;
        }
        did_read = TRUE;
        red_char_device_send_msg_to_clients(dev, msg.get());
        max_send_tokens--;
    }
    dev->priv->during_read_from_device = 0;
    if (dev->priv->running) {
        dev->priv->active = dev->priv->active || did_read;
    }
    return did_read;
}

static void red_char_device_client_send_queue_push(RedCharDeviceClient *dev_client)
{
    while (!dev_client->send_queue.empty() &&
           red_char_device_can_send_to_client(dev_client)) {
        RedPipeItemPtr msg = std::move(dev_client->send_queue.back());
        dev_client->send_queue.pop_back();
        g_assert(msg);
        dev_client->num_send_tokens--;
        dev_client->dev->send_msg_to_client(msg.get(), dev_client->client);
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

    if (!dev_client->send_queue.empty()) {
        spice_assert(dev_client->num_send_tokens == tokens);
        red_char_device_client_send_queue_push(dev_client);
    }

    if (red_char_device_can_send_to_client(dev_client)) {
        red_timer_cancel(dev_client->wait_for_tokens_timer);
        dev_client->wait_for_tokens_started = FALSE;
        red_char_device_read_from_device(dev_client->dev);
    } else if (!dev_client->send_queue.empty()) {
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
        dev->send_tokens_to_client(dev_client->client, tokens);
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

    red::shared_ptr<RedCharDevice> hold_dev(this);

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
        return nullptr;
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
    return nullptr;
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
    return  red_char_device_write_buffer_get(this, nullptr, size, origin, 0);
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
    *p_write_buf = nullptr;

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
        dev->on_free_self_token();
    }
}

/********************************
 * char_device_state management *
 ********************************/

void RedCharDevice::reset_dev_instance(SpiceCharDeviceInstance *sin)
{
    spice_debug("sin %p, char device %p", sin, this);
    priv->sin = sin;
    if (sin) {
        sin->st = this;
    }
    if (priv->reds) {
        init_device_instance();
    }
}

RedCharDeviceClient::RedCharDeviceClient(RedCharDevice *init_dev,
                                         RedsState *reds,
                                         RedCharDeviceClientOpaque *init_client,
                                         bool init_do_flow_control,
                                         uint32_t init_max_send_queue_size,
                                         uint32_t init_num_client_tokens,
                                         uint32_t init_num_send_tokens):
    dev(init_dev),
    client(init_client),
    do_flow_control(init_do_flow_control),
    max_send_queue_size(init_max_send_queue_size)
{
    if (do_flow_control) {
        wait_for_tokens_timer =
            reds_core_timer_add(reds, device_client_wait_for_tokens_timeout, this);
        if (!wait_for_tokens_timer) {
            spice_error("failed to create wait for tokens timer");
        }
        num_client_tokens = init_num_client_tokens;
        num_send_tokens = init_num_send_tokens;
    } else {
        num_client_tokens = ~0;
        num_send_tokens = ~0;
    }
}

RedCharDeviceClient::~RedCharDeviceClient() = default;

bool RedCharDevice::client_add(RedCharDeviceClientOpaque *client,
                               int do_flow_control,
                               uint32_t max_send_queue_size,
                               uint32_t num_client_tokens,
                               uint32_t num_send_tokens,
                               int wait_for_migrate_data)
{
    RedCharDeviceClient *dev_client;


    spice_assert(client);

    if (wait_for_migrate_data && (priv->clients != nullptr || priv->active)) {
        spice_warning("can't restore device %p from migration data. The device "
                      "has already been active", this);
        return FALSE;
    }

    priv->wait_for_migrate_data = wait_for_migrate_data;

    spice_debug("char device %p, client %p", this, client);
    dev_client = new RedCharDeviceClient(this,
                                         priv->reds,
                                         client,
                                         !!do_flow_control,
                                         max_send_queue_size,
                                         num_client_tokens,
                                         num_send_tokens);
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
        spice_assert(priv->clients == nullptr);
        priv->wait_for_migrate_data  = FALSE;
        red_char_device_read_from_device(this);
    }
}

int RedCharDevice::client_exists(RedCharDeviceClientOpaque *client)
{
    return (red_char_device_client_find(this, client) != nullptr);
}

void RedCharDevice::start()
{
    spice_debug("char device %p", this);
    priv->running = TRUE;
    red::shared_ptr<RedCharDevice> hold_dev(this);
    while (write_to_device() ||
           red_char_device_read_from_device(this));
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
        spice_debug("send_queue_empty %d", dev_client->send_queue.empty());
        dev_client->num_send_tokens += dev_client->send_queue.size();
        dev_client->send_queue.clear();

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
    auto write_buf = (RedCharDeviceWriteBuffer *)opaque;

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
    spice_assert(dev_client->send_queue.empty());
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

    for (item = g_queue_peek_tail_link(&priv->write_queue); item != nullptr; item = item->prev) {
        auto write_buf = (RedCharDeviceWriteBuffer *) item->data;

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
                red_char_device_write_buffer_get(this, nullptr,
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


void RedCharDevice::init_device_instance()
{
    SpiceCharDeviceInterface *sif;

    g_return_if_fail(priv->reds);

    red_timer_remove(priv->write_to_dev_timer);
    priv->write_to_dev_timer = nullptr;

    if (priv->sin == nullptr) {
       return;
    }

    sif = spice_char_device_get_interface(priv->sin);
    if (sif->base.minor_version <= 2 ||
        !(sif->flags & SPICE_CHAR_DEVICE_NOTIFY_WRITABLE)) {
        priv->write_to_dev_timer = reds_core_timer_add(priv->reds,
                                                       RedCharDevice::write_retry,
                                                       this);
        if (!priv->write_to_dev_timer) {
            spice_error("failed creating char dev write timer");
        }
    }

    priv->sin->st = this;
}

RedCharDevice::~RedCharDevice()
{
    red_timer_remove(priv->write_to_dev_timer);
    priv->write_to_dev_timer = nullptr;

    write_buffers_queue_free(&priv->write_queue);
    red_char_device_write_buffer_free(priv->cur_write_buf);
    priv->cur_write_buf = nullptr;

    while (priv->clients != nullptr) {
        auto dev_client = (RedCharDeviceClient *) priv->clients->data;
        red_char_device_client_free(this, dev_client);
    }
    priv->running = FALSE;
}

void
RedCharDevice::port_event(uint8_t event)
{
}

SPICE_GNUC_VISIBLE void spice_server_port_event(SpiceCharDeviceInstance *sin, uint8_t event)
{
    if (sin->st == nullptr) {
        spice_warning("no RedCharDevice attached to instance %p", sin);
        return;
    }

    sin->st->port_event(event);
}

SpiceCharDeviceInstance *RedCharDevice::get_device_instance()
{
    return priv->sin;
}

RedCharDevice::RedCharDevice(RedsState *reds, SpiceCharDeviceInstance *sin,
                             uint64_t client_tokens_interval, uint64_t num_self_tokens)
{
    priv->reds = reds;
    priv->client_tokens_interval = client_tokens_interval;
    priv->num_self_tokens = num_self_tokens;
    reset_dev_instance(sin);

    g_queue_init(&priv->write_queue);
}

int RedCharDevice::read(uint8_t *buf, int len)
{
    auto sif = spice_char_device_get_interface(priv->sin);

    int ret = sif->read(priv->sin, buf, len);
    if (ret > 0) {
        priv->active = true;
    }
    return ret;
}
