/*
   Copyright (C) 2009-2016 Red Hat, Inc.

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

#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#ifndef _WIN32
#include <poll.h>
#endif

#include "dispatcher.h"

#define DISPATCHER_MESSAGE_TYPE_CUSTOM 0x7fffffffu

/* structure to store message header information.
 * That structure is sent through a socketpair so it's optimized
 * to be transfered via sockets.
 * Is also packaged to not leave holes in both 32 and 64 environments
 * so memory instrumentation tools should not find uninitialised bytes.
 */
struct DispatcherMessage {
    dispatcher_handle_message handler;
    uint32_t size;
    uint32_t type:31;
    uint32_t ack:1;
};

struct DispatcherPrivate {
    SPICE_CXX_GLIB_ALLOCATOR
    explicit DispatcherPrivate(uint32_t init_max_message_type):
        max_message_type(init_max_message_type)
    {
    }
    ~DispatcherPrivate();
    void send_message(const DispatcherMessage& msg, void *payload);
    bool handle_single_read();
    static void handle_event(int fd, int event, DispatcherPrivate* priv);

    int recv_fd;
    int send_fd;
    pthread_mutex_t lock;
    DispatcherMessage *messages;
    const guint max_message_type;
    void *payload; /* allocated as max of message sizes */
    size_t payload_size; /* used to track realloc calls */
    void *opaque;
    dispatcher_handle_any_message any_handler;
};

DispatcherPrivate::~DispatcherPrivate()
{
    g_free(messages);
    socket_close(send_fd);
    socket_close(recv_fd);
    pthread_mutex_destroy(&lock);
    g_free(payload);
}

Dispatcher::~Dispatcher() = default;

Dispatcher::Dispatcher(uint32_t max_message_type):
    priv(new DispatcherPrivate(max_message_type))
{
    int channels[2];

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, channels) == -1) {
        spice_error("socketpair failed %s", strerror(errno));
        return;
    }
    pthread_mutex_init(&priv->lock, nullptr);
    priv->recv_fd = channels[0];
    priv->send_fd = channels[1];

    priv->messages = g_new0(DispatcherMessage, priv->max_message_type);
}

#define ACK 0xffffffff

/*
 * read_safe
 * helper. reads until size bytes accumulated in buf, if an error other then
 * EINTR is encountered returns -1, otherwise returns 0.
 * @block if 1 the read will block (the fd is always blocking).
 *        if 0 poll first, return immediately if no bytes available, otherwise
 *         read size in blocking mode.
 */
static int read_safe(int fd, uint8_t *buf, size_t size, int block)
{
    int read_size = 0;
    int ret;

    if (size == 0) {
        return 0;
    }

    if (!block) {
#ifndef _WIN32
        struct pollfd pollfd = {.fd = fd, .events = POLLIN, .revents = 0};
        while ((ret = poll(&pollfd, 1, 0)) == -1) {
            if (errno == EINTR) {
                spice_debug("EINTR in poll");
                continue;
            }
            spice_error("poll failed");
            return -1;
        }
        if (!(pollfd.revents & POLLIN)) {
            return 0;
        }
#else
        struct timeval tv = { 0, 0 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        if (select(1, &fds, NULL, NULL, &tv) < 1) {
            return 0;
        }
#endif
    }
    while (read_size < size) {
        ret = socket_read(fd, buf + read_size, size - read_size);
        if (ret == -1) {
            if (errno == EINTR) {
                spice_debug("EINTR in read");
                continue;
            }
#ifdef _WIN32
            // Windows turns this socket not-blocking
            if (errno == EAGAIN) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(fd, &fds);
                select(1, &fds, NULL, NULL, NULL);
                continue;
            }
#endif
            return -1;
        }
        if (ret == 0) {
            spice_error("broken pipe on read");
            return -1;
        }
        read_size += ret;
    }
    return read_size;
}

/*
 * write_safe
 * @return -1 for error, otherwise number of written bytes. may be zero.
 */
static int write_safe(int fd, uint8_t *buf, size_t size)
{
    int written_size = 0;
    int ret;

    while (written_size < size) {
        ret = socket_write(fd, buf + written_size, size - written_size);
        if (ret == -1) {
            if (errno != EINTR) {
                return -1;
            }
            spice_debug("EINTR in write");
            continue;
        }
        written_size += ret;
    }
    return written_size;
}

bool DispatcherPrivate::handle_single_read()
{
    int ret;
    DispatcherMessage msg[1];
    uint32_t ack = ACK;

    if ((ret = read_safe(recv_fd, (uint8_t*)msg, sizeof(msg), 0)) == -1) {
        g_warning("error reading from dispatcher: %d", errno);
        return false;
    }
    if (ret == 0) {
        /* no message */
        return false;
    }
    if (G_UNLIKELY(msg->size > payload_size)) {
        payload = g_realloc(payload, msg->size);
        payload_size = msg->size;
    }
    if (read_safe(recv_fd, (uint8_t*) payload, msg->size, 1) == -1) {
        g_warning("error reading from dispatcher: %d", errno);
        /* TODO: close socketpair? */
        return false;
    }
    if (any_handler && msg->type != DISPATCHER_MESSAGE_TYPE_CUSTOM) {
        any_handler(opaque, msg->type, payload);
    }
    if (msg->handler) {
        msg->handler(opaque, payload);
    } else {
        g_warning("error: no handler for message type %d", msg->type);
    }
    if (msg->ack) {
        if (write_safe(recv_fd, (uint8_t*)&ack, sizeof(ack)) == -1) {
            g_warning("error writing ack for message %d", msg->type);
            /* TODO: close socketpair? */
        }
    }
    return true;
}

/*
 * handle_event
 * doesn't handle being in the middle of a message. all reads are blocking.
 */
void DispatcherPrivate::handle_event(int fd, int event, DispatcherPrivate* priv)
{
    while (priv->handle_single_read()) {
    }
}

void DispatcherPrivate::send_message(const DispatcherMessage& msg, void *msg_payload)
{
    uint32_t ack;

    pthread_mutex_lock(&lock);
    if (write_safe(send_fd, (uint8_t*)&msg, sizeof(msg)) == -1) {
        g_warning("error: failed to send message header for message %d",
                  msg.type);
        goto unlock;
    }
    if (write_safe(send_fd, (uint8_t*) msg_payload, msg.size) == -1) {
        g_warning("error: failed to send message body for message %d",
                  msg.type);
        goto unlock;
    }
    if (msg.ack) {
        if (read_safe(send_fd, (uint8_t*)&ack, sizeof(ack), 1) == -1) {
            g_warning("error: failed to read ack");
        } else if (ack != ACK) {
            g_warning("error: got wrong ack value in dispatcher "
                      "for message %d\n", msg.type);
            /* TODO handling error? */
        }
    }
unlock:
    pthread_mutex_unlock(&lock);
}

void Dispatcher::send_message(uint32_t message_type, void *payload)
{
    assert(priv->max_message_type > message_type);
    assert(priv->messages[message_type].handler);
    priv->send_message(priv->messages[message_type], payload);
}

void Dispatcher::send_message_custom(dispatcher_handle_message handler,
                                     void *payload, uint32_t payload_size, bool ack)
{
    DispatcherMessage msg = {
        .handler = handler,
        .size = payload_size,
        .type = DISPATCHER_MESSAGE_TYPE_CUSTOM,
        .ack = ack,
    };
    priv->send_message(msg, payload);
}

void Dispatcher::register_handler(uint32_t message_type,
                                  dispatcher_handle_message handler,
                                  size_t size, bool ack)
{
    DispatcherMessage *msg;

    assert(message_type < priv->max_message_type);
    assert(priv->messages[message_type].handler == nullptr);
    msg = &priv->messages[message_type];
    msg->handler = handler;
    msg->size = size;
    msg->type = message_type;
    msg->ack = ack;
    if (msg->size > priv->payload_size) {
        priv->payload = g_realloc(priv->payload, msg->size);
        priv->payload_size = msg->size;
    }
}

void Dispatcher::register_universal_handler(dispatcher_handle_any_message any_handler)
{
    priv->any_handler = any_handler;
}

SpiceWatch *Dispatcher::create_watch(SpiceCoreInterfaceInternal *core)
{
    return core->watch_new(priv->recv_fd,
                           SPICE_WATCH_EVENT_READ, DispatcherPrivate::handle_event, priv.get());
}

void Dispatcher::set_opaque(void *opaque)
{
    priv->opaque = opaque;
}
