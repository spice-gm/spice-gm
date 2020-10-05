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

#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#endif
#ifdef HAVE_LINUX_SOCKIOS_H
#include <linux/sockios.h> /* SIOCOUTQ */
#endif
#include <common/generated_server_marshallers.h>

#include "red-channel-client.h"
#include "red-client.h"

#define CLIENT_ACK_WINDOW 20

#define MAX_HEADER_SIZE sizeof(SpiceDataHeader)

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

struct SpiceDataHeaderOpaque;

typedef uint16_t (*get_msg_type_proc)(SpiceDataHeaderOpaque *header);
typedef uint32_t (*get_msg_size_proc)(SpiceDataHeaderOpaque *header);
typedef void (*set_msg_type_proc)(SpiceDataHeaderOpaque *header, uint16_t type);
typedef void (*set_msg_size_proc)(SpiceDataHeaderOpaque *header, uint32_t size);
typedef void (*set_msg_serial_proc)(SpiceDataHeaderOpaque *header, uint64_t serial);
typedef void (*set_msg_sub_list_proc)(SpiceDataHeaderOpaque *header, uint32_t sub_list);

struct SpiceDataHeaderOpaque {
    uint8_t *data;
    uint16_t header_size;

    set_msg_type_proc set_msg_type;
    set_msg_size_proc set_msg_size;
    set_msg_serial_proc set_msg_serial;
    set_msg_sub_list_proc set_msg_sub_list;

    get_msg_type_proc get_msg_type;
    get_msg_size_proc get_msg_size;
};

enum QosPingState {
    PING_STATE_NONE,
    PING_STATE_TIMER,
    PING_STATE_WARMUP,
    PING_STATE_LATENCY,
};

struct RedChannelClientLatencyMonitor {
    QosPingState state;
    uint64_t last_pong_time;
    SpiceTimer *timer;
    uint32_t timeout;
    uint32_t id;
    bool tcp_nodelay;
    bool warmup_was_sent;

    int64_t roundtrip;
};

enum ConnectivityState {
    CONNECTIVITY_STATE_CONNECTED,
    CONNECTIVITY_STATE_BLOCKED,
    CONNECTIVITY_STATE_WAIT_PONG,
    CONNECTIVITY_STATE_DISCONNECTED,
};

struct RedChannelClientConnectivityMonitor {
    ConnectivityState state;
    bool sent_bytes;
    bool received_bytes;
    uint32_t timeout;
    SpiceTimer *timer;
};

struct OutgoingMessageBuffer {
    int pos;
    int size;
};

struct IncomingMessageBuffer {
    uint8_t header_buf[MAX_HEADER_SIZE];
    SpiceDataHeaderOpaque header;
    uint32_t header_pos;
    uint8_t *msg; // data of the msg following the header. allocated by alloc_msg_buf.
    uint32_t msg_pos;
};

struct RedChannelClientPrivate
{
    SPICE_CXX_GLIB_ALLOCATOR

    RedChannelClientPrivate(RedChannel *channel,
                            RedClient *client,
                            RedStream *stream,
                            RedChannelCapabilities *caps,
                            bool monitor_latency);
    ~RedChannelClientPrivate();

    red::shared_ptr<RedChannel> channel;
    RedClient *const client;
    RedStream *const stream;
    bool monitor_latency;

    struct {
        uint32_t generation;
        uint32_t client_generation;
        uint32_t messages_window;
        uint32_t client_window;
    } ack_data;

    struct {
        /* this can be either main.marshaller or urgent.marshaller */
        SpiceMarshaller *marshaller;
        SpiceDataHeaderOpaque header;
        uint32_t size;
        bool blocked;
        uint64_t last_sent_serial;

        struct {
            SpiceMarshaller *marshaller;
            uint8_t *header_data;
        } main;

        struct {
            SpiceMarshaller *marshaller;
        } urgent;
    } send_data;

    bool block_read;
    bool during_send;
    RedChannelClient::Pipe pipe;

    RedChannelCapabilities remote_caps;
    bool is_mini_header;

    bool wait_migrate_data;
    bool wait_migrate_flush_mark;

    RedChannelClientLatencyMonitor latency_monitor;
    RedChannelClientConnectivityMonitor connectivity_monitor;

    IncomingMessageBuffer incoming;
    OutgoingMessageBuffer outgoing;

    RedStatCounter out_messages;
    RedStatCounter out_bytes;

    inline RedPipeItemPtr pipe_item_get();
    inline void pipe_remove(RedPipeItem *item);
    void handle_pong(SpiceMsgPing *ping);
    inline void set_message_serial(uint64_t serial);
    void pipe_clear();
    void data_sent(int n);
    void data_read(int n);
    inline int get_out_msg_size();
    inline int prepare_out_msg(struct iovec *vec, int vec_size, int pos);
    inline void set_blocked();
    void reset_send_data();
    void seamless_migration_done();
    void clear_sent_item();
    void restart_ping_timer();
    void start_ping_timer(uint32_t timeout);
    void cancel_ping_timer();
    inline int urgent_marshaller_is_active();
    inline int waiting_for_ack();
    inline void restore_main_sender();
    void watch_update_mask(int event_mask);
};

static void full_header_set_msg_type(SpiceDataHeaderOpaque *header, uint16_t type);
static void full_header_set_msg_size(SpiceDataHeaderOpaque *header, uint32_t size);
static void full_header_set_msg_serial(SpiceDataHeaderOpaque *header, uint64_t serial);
static void full_header_set_msg_sub_list(SpiceDataHeaderOpaque *header, uint32_t sub_list);
static uint16_t full_header_get_msg_type(SpiceDataHeaderOpaque *header);
static uint32_t full_header_get_msg_size(SpiceDataHeaderOpaque *header);

static const SpiceDataHeaderOpaque full_header_wrapper = {nullptr, sizeof(SpiceDataHeader),
                                                          full_header_set_msg_type,
                                                          full_header_set_msg_size,
                                                          full_header_set_msg_serial,
                                                          full_header_set_msg_sub_list,
                                                          full_header_get_msg_type,
                                                          full_header_get_msg_size};

static void mini_header_set_msg_type(SpiceDataHeaderOpaque *header, uint16_t type);
static void mini_header_set_msg_size(SpiceDataHeaderOpaque *header, uint32_t size);
static void mini_header_set_msg_serial(SpiceDataHeaderOpaque *header, uint64_t serial);
static void mini_header_set_msg_sub_list(SpiceDataHeaderOpaque *header, uint32_t sub_list);
static uint16_t mini_header_get_msg_type(SpiceDataHeaderOpaque *header);
static uint32_t mini_header_get_msg_size(SpiceDataHeaderOpaque *header);

static const SpiceDataHeaderOpaque mini_header_wrapper = {nullptr, sizeof(SpiceMiniDataHeader),
                                                          mini_header_set_msg_type,
                                                          mini_header_set_msg_size,
                                                          mini_header_set_msg_serial,
                                                          mini_header_set_msg_sub_list,
                                                          mini_header_get_msg_type,
                                                          mini_header_get_msg_size};

/*
 * When an error occurs over a channel, we treat it as a warning
 * for spice-server and shutdown the channel.
 */
#define spice_channel_client_error(rcc, ...)                                             \
    do {                                                                                 \
        red_channel_warning(rcc->priv->channel, __VA_ARGS__);                            \
        rcc->shutdown();                                                                 \
    } while (0)

#define PING_TEST_TIMEOUT_MS (MSEC_PER_SEC * 15)
#define PING_TEST_LONG_TIMEOUT_MS (MSEC_PER_SEC * 60 * 5)
#define PING_TEST_IDLE_NET_TIMEOUT_MS (MSEC_PER_SEC / 10)

struct RedEmptyMsgPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_EMPTY_MSG> {
    int msg;
};

struct MarkerPipeItem: public RedPipeItemNum<RED_PIPE_ITEM_TYPE_MARKER> {
    bool item_sent;
};

void RedChannelClientPrivate::start_ping_timer(uint32_t timeout)
{
    if (!latency_monitor.timer) {
        return;
    }
    if (latency_monitor.state != PING_STATE_NONE) {
        return;
    }
    latency_monitor.state = PING_STATE_TIMER;

    red_timer_start(latency_monitor.timer, timeout);
}

void RedChannelClientPrivate::cancel_ping_timer()
{
    if (!latency_monitor.timer) {
        return;
    }
    if (latency_monitor.state != PING_STATE_TIMER) {
        return;
    }

    red_timer_cancel(latency_monitor.timer);
    latency_monitor.state = PING_STATE_NONE;
}

void RedChannelClientPrivate::restart_ping_timer()
{
    uint64_t passed, timeout;

    if (!latency_monitor.timer) {
        return;
    }
    passed = (spice_get_monotonic_time_ns() - latency_monitor.last_pong_time) / NSEC_PER_MILLISEC;
    timeout = PING_TEST_IDLE_NET_TIMEOUT_MS;
    if (passed  < latency_monitor.timeout) {
        timeout += latency_monitor.timeout - passed;
    }

    start_ping_timer(timeout);
}

RedChannelClientPrivate::RedChannelClientPrivate(RedChannel *init_channel,
                                                 RedClient *init_client,
                                                 RedStream *init_stream,
                                                 RedChannelCapabilities *caps,
                                                 bool init_monitor_latency):
    channel(init_channel),
    client(init_client), stream(init_stream),
    monitor_latency(init_monitor_latency)
{
    // blocks send message (maybe use send_data.blocked + block flags)
    ack_data.messages_window = ~0;
    ack_data.client_generation = ~0;
    ack_data.client_window = CLIENT_ACK_WINDOW;
    send_data.main.marshaller = spice_marshaller_new();
    send_data.urgent.marshaller = spice_marshaller_new();

    send_data.marshaller = send_data.main.marshaller;

    red_channel_capabilities_reset(&remote_caps);
    red_channel_capabilities_init(&remote_caps, caps);

    outgoing.pos = 0;
    outgoing.size = 0;

    if (test_capability(remote_caps.common_caps, remote_caps.num_common_caps,
                        SPICE_COMMON_CAP_MINI_HEADER)) {
        incoming.header = mini_header_wrapper;
        send_data.header = mini_header_wrapper;
        is_mini_header = TRUE;
    } else {
        incoming.header = full_header_wrapper;
        send_data.header = full_header_wrapper;
        is_mini_header = FALSE;
    }
    incoming.header.data = incoming.header_buf;

    RedsState* reds = channel->get_server();
    const RedStatNode *node = channel->get_stat_node();
    stat_init_counter(&out_messages, reds, node, "out_messages", TRUE);
    stat_init_counter(&out_bytes, reds, node, "out_bytes", TRUE);
}

RedChannelClientPrivate::~RedChannelClientPrivate()
{
    red_timer_remove(latency_monitor.timer);
    latency_monitor.timer = nullptr;

    red_timer_remove(connectivity_monitor.timer);
    connectivity_monitor.timer = nullptr;

    red_stream_free(stream);

    if (send_data.main.marshaller) {
        spice_marshaller_destroy(send_data.main.marshaller);
    }

    if (send_data.urgent.marshaller) {
        spice_marshaller_destroy(send_data.urgent.marshaller);
    }

    red_channel_capabilities_reset(&remote_caps);
}

/* This even empty is better to by declared here to make sure
 * we call the right delete for priv field
 */
RedChannelClient::~RedChannelClient() = default;

RedChannelClient::RedChannelClient(RedChannel *channel,
                                   RedClient *client,
                                   RedStream *stream,
                                   RedChannelCapabilities *caps,
                                   bool monitor_latency):
    priv(new RedChannelClientPrivate(channel, client, stream, caps, monitor_latency))
{
}

RedChannel* RedChannelClient::get_channel()
{
    return priv->channel.get();
}

void RedChannelClientPrivate::data_sent(int n)
{
    if (connectivity_monitor.timer) {
        connectivity_monitor.sent_bytes = true;
    }
    stat_inc_counter(out_bytes, n);
}

void RedChannelClientPrivate::data_read(int n)
{
    if (connectivity_monitor.timer) {
        connectivity_monitor.received_bytes = true;
    }
}

inline int RedChannelClientPrivate::get_out_msg_size()
{
    return send_data.size;
}

inline int RedChannelClientPrivate::prepare_out_msg(struct iovec *vec, int vec_size, int pos)
{
    return spice_marshaller_fill_iovec(send_data.marshaller,
                                       vec, vec_size, pos);
}

inline void RedChannelClientPrivate::set_blocked()
{
    send_data.blocked = true;
}

inline int RedChannelClientPrivate::urgent_marshaller_is_active()
{
    return send_data.marshaller == send_data.urgent.marshaller;
}

void RedChannelClientPrivate::reset_send_data()
{
    spice_marshaller_reset(send_data.marshaller);
    send_data.header.data = spice_marshaller_reserve_space(send_data.marshaller,
                                                           send_data.header.header_size);
    spice_marshaller_set_base(send_data.marshaller, send_data.header.header_size);
    send_data.header.set_msg_type(&send_data.header, 0);
    send_data.header.set_msg_size(&send_data.header, 0);

    if (!is_mini_header) {
        spice_assert(send_data.marshaller != send_data.urgent.marshaller);
        send_data.header.set_msg_sub_list(&send_data.header, 0);
    }
}

void RedChannelClient::send_set_ack()
{
    SpiceMsgSetAck ack;

    init_send_data(SPICE_MSG_SET_ACK);
    ack.generation = ++priv->ack_data.generation;
    ack.window = priv->ack_data.client_window;
    priv->ack_data.messages_window = 0;

    spice_marshall_msg_set_ack(priv->send_data.marshaller, &ack);

    begin_send_message();
}

void RedChannelClient::send_migrate()
{
    SpiceMsgMigrate migrate;

    init_send_data(SPICE_MSG_MIGRATE);
    migrate.flags = priv->channel->migration_flags();
    spice_marshall_msg_migrate(priv->send_data.marshaller, &migrate);
    if (migrate.flags & SPICE_MIGRATE_NEED_FLUSH) {
        priv->wait_migrate_flush_mark = TRUE;
    }

    begin_send_message();
}

void RedChannelClient::send_ping()
{
    SpiceMsgPing ping;

    if (!priv->latency_monitor.warmup_was_sent) { // latency test start
        int delay_val;

        priv->latency_monitor.warmup_was_sent = true;
        /*
         * When testing latency, TCP_NODELAY must be switched on, otherwise,
         * sending the ping message is delayed by Nagle algorithm, and the
         * roundtrip measurement is less accurate (bigger).
         */
        priv->latency_monitor.tcp_nodelay = true;
        delay_val = red_stream_get_no_delay(priv->stream);
        if (delay_val != -1) {
            priv->latency_monitor.tcp_nodelay = delay_val;
            if (!delay_val) {
                red_stream_set_no_delay(priv->stream, TRUE);
            }
        }
    }

    init_send_data(SPICE_MSG_PING);
    ping.id = priv->latency_monitor.id;
    ping.timestamp = spice_get_monotonic_time_ns();
    spice_marshall_msg_ping(priv->send_data.marshaller, &ping);
    begin_send_message();
}

void RedChannelClient::send_empty_msg(RedPipeItem *base)
{
    auto msg_pipe_item = static_cast<RedEmptyMsgPipeItem*>(base);

    init_send_data(msg_pipe_item->msg);
    begin_send_message();
}

void RedChannelClient::send_any_item(RedPipeItem *item)
{
    spice_assert(no_item_being_sent());
    priv->reset_send_data();
    switch (item->type) {
        case RED_PIPE_ITEM_TYPE_SET_ACK:
            send_set_ack();
            break;
        case RED_PIPE_ITEM_TYPE_MIGRATE:
            send_migrate();
            break;
        case RED_PIPE_ITEM_TYPE_EMPTY_MSG:
            send_empty_msg(item);
            break;
        case RED_PIPE_ITEM_TYPE_PING:
            send_ping();
            break;
        case RED_PIPE_ITEM_TYPE_MARKER:
            static_cast<MarkerPipeItem*>(item)->item_sent = true;
            break;
        default:
            send_item(item);
            break;
    }
}

inline void RedChannelClientPrivate::restore_main_sender()
{
    send_data.marshaller = send_data.main.marshaller;
    send_data.header.data = send_data.main.header_data;
}

void RedChannelClient::msg_sent()
{
#ifndef _WIN32
    int fd;

    if (spice_marshaller_get_fd(priv->send_data.marshaller, &fd)) {
        if (red_stream_send_msgfd(priv->stream, fd) < 0) {
            perror("sendfd");
            disconnect();
            if (fd != -1)
                close(fd);
            return;
        }
        if (fd != -1)
            close(fd);
    }
#endif

    priv->clear_sent_item();

    if (priv->urgent_marshaller_is_active()) {
        priv->restore_main_sender();
        spice_assert(priv->send_data.header.data != nullptr);
        begin_send_message();
    } else {
        if (priv->pipe.empty()) {
            /* It is possible that the socket will become idle, so we may be able to test latency */
            priv->restart_ping_timer();
        }
    }

}

static RedChannelClient::Pipe::iterator
find_pipe_item(RedChannelClient::Pipe &pipe, const RedPipeItem *item)
{
    return std::find_if(pipe.begin(), pipe.end(),
                        [=](const RedPipeItemPtr& p) -> bool {
                            return p.get() == item;
    });
}

static RedChannelClient::Pipe::const_iterator
find_pipe_item(const RedChannelClient::Pipe &pipe, const RedPipeItem *item)
{
    return std::find_if(pipe.begin(), pipe.end(),
                        [=](const RedPipeItemPtr& p) -> bool {
                            return p.get() == item;
    });
}

void RedChannelClientPrivate::pipe_remove(RedPipeItem *item)
{
    auto i = find_pipe_item(pipe, item);
    if (i != pipe.end()) {
        pipe.erase(i);
    }
}

bool RedChannelClient::test_remote_common_cap(uint32_t cap) const
{
    return test_capability(priv->remote_caps.common_caps,
                           priv->remote_caps.num_common_caps,
                           cap);
}

bool RedChannelClient::test_remote_cap(uint32_t cap) const
{
    return test_capability(priv->remote_caps.caps,
                           priv->remote_caps.num_caps,
                           cap);
}

void RedChannelClient::push_ping()
{
    spice_assert(priv->latency_monitor.state == PING_STATE_NONE);
    priv->latency_monitor.state = PING_STATE_WARMUP;
    priv->latency_monitor.warmup_was_sent = false;
    priv->latency_monitor.id = rand();
    pipe_add_type(RED_PIPE_ITEM_TYPE_PING);
    pipe_add_type(RED_PIPE_ITEM_TYPE_PING);
}

void RedChannelClient::ping_timer(RedChannelClient *rcc)
{
    red::shared_ptr<RedChannelClient> hold_rcc(rcc);
    spice_assert(rcc->priv->latency_monitor.state == PING_STATE_TIMER);
    rcc->priv->cancel_ping_timer();

#ifdef HAVE_LINUX_SOCKIOS_H /* SIOCOUTQ is a Linux only ioctl on sockets. */
    int so_unsent_size = 0;

    /* retrieving the occupied size of the socket's tcp send buffer (unacked + unsent) */
    if (ioctl(rcc->priv->stream->socket, SIOCOUTQ, &so_unsent_size) == -1) {
        red_channel_warning(rcc->get_channel(),
                            "ioctl(SIOCOUTQ) failed, %s", strerror(errno));
    }
    if (so_unsent_size > 0) {
        /* tcp send buffer is still occupied. rescheduling ping */
        rcc->priv->start_ping_timer(PING_TEST_IDLE_NET_TIMEOUT_MS);
        return;
    }
#endif /* ifdef HAVE_LINUX_SOCKIOS_H */
    /* More portable alternative code path (less accurate but avoids bogus ioctls)*/
    rcc->push_ping();
}

inline int RedChannelClientPrivate::waiting_for_ack()
{
    gboolean handle_acks = channel->handle_acks();

    return (handle_acks && (ack_data.messages_window >
                            ack_data.client_window * 2));
}

/*
 * When a connection is not alive (and we can't detect it via a socket error), we
 * reach one of these 2 states:
 * (1) Sending msgs is blocked: either writes return EAGAIN
 *     or we are missing MSGC_ACK from the client.
 * (2) MSG_PING was sent without receiving a MSGC_PONG in reply.
 *
 * The connectivity_timer callback tests if the channel's state matches one of the above.
 * In case it does, on the next time the timer is called, it checks if the connection has
 * been idle during the time that passed since the previous timer call. If the connection
 * has been idle, we consider the client as disconnected.
 */
void RedChannelClient::connectivity_timer(RedChannelClient *rcc)
{
    RedChannelClientConnectivityMonitor *monitor = &rcc->priv->connectivity_monitor;
    int is_alive = TRUE;

    red::shared_ptr<RedChannelClient> hold_rcc(rcc);

    if (monitor->state == CONNECTIVITY_STATE_BLOCKED) {
        if (!monitor->received_bytes && !monitor->sent_bytes) {
            if (!rcc->is_blocked() && !rcc->priv->waiting_for_ack()) {
                spice_error("mismatch between rcc-state and connectivity-state");
            }
            spice_debug("rcc is blocked; connection is idle");
            is_alive = FALSE;
        }
    } else if (monitor->state == CONNECTIVITY_STATE_WAIT_PONG) {
        if (!monitor->received_bytes) {
            if (rcc->priv->latency_monitor.state != PING_STATE_WARMUP &&
                rcc->priv->latency_monitor.state != PING_STATE_LATENCY) {
                spice_error("mismatch between rcc-state and connectivity-state");
            }
            spice_debug("rcc waits for pong; connection is idle");
            is_alive = FALSE;
        }
    }

    if (is_alive) {
        monitor->received_bytes = false;
        monitor->sent_bytes = false;
        if (rcc->is_blocked() || rcc->priv->waiting_for_ack()) {
            monitor->state = CONNECTIVITY_STATE_BLOCKED;
        } else if (rcc->priv->latency_monitor.state == PING_STATE_WARMUP ||
                   rcc->priv->latency_monitor.state == PING_STATE_LATENCY) {
            monitor->state = CONNECTIVITY_STATE_WAIT_PONG;
        } else {
            monitor->state = CONNECTIVITY_STATE_CONNECTED;
        }
        red_timer_start(monitor->timer, monitor->timeout);
    } else {
        monitor->state = CONNECTIVITY_STATE_DISCONNECTED;
        red_channel_warning(rcc->priv->channel.get(),
                            "rcc %p has been unresponsive for more than %u ms, disconnecting",
                            rcc, monitor->timeout);
        rcc->disconnect();
    }
}

void RedChannelClient::start_connectivity_monitoring(uint32_t timeout_ms)
{
    SpiceCoreInterfaceInternal *core = priv->channel->get_core_interface();
    if (!is_connected()) {
        return;
    }
    spice_debug("trace");
    spice_assert(timeout_ms > 0);
    /*
     * If latency_monitor is not active, we activate it in order to enable
     * periodic ping messages so that we will be be able to identify a disconnected
     * channel-client even if there are no ongoing channel specific messages
     * on this channel.
     */
    if (priv->latency_monitor.timer == nullptr) {
        priv->latency_monitor.timer =
            core->timer_new(ping_timer, this);
        priv->latency_monitor.roundtrip = -1;
    } else {
        priv->cancel_ping_timer();
    }
    priv->latency_monitor.timeout = PING_TEST_TIMEOUT_MS;
    if (!priv->client->during_migrate_at_target()) {
        priv->start_ping_timer(PING_TEST_IDLE_NET_TIMEOUT_MS);
    }
    if (priv->connectivity_monitor.timer == nullptr) {
        priv->connectivity_monitor.state = CONNECTIVITY_STATE_CONNECTED;
        priv->connectivity_monitor.timer =
            core->timer_new(connectivity_timer, this);
        priv->connectivity_monitor.timeout = timeout_ms;
        if (!priv->client->during_migrate_at_target()) {
            red_timer_start(priv->connectivity_monitor.timer,
                            priv->connectivity_monitor.timeout);
        }
    }
}

static void red_channel_client_event(int fd, int event, RedChannelClient *rcc)
{
    red::shared_ptr<RedChannelClient> hold_rcc(rcc);
    if (event & SPICE_WATCH_EVENT_READ) {
        rcc->receive();
    }
    if (event & SPICE_WATCH_EVENT_WRITE) {
        rcc->push();
    }
}

static uint32_t full_header_get_msg_size(SpiceDataHeaderOpaque *header)
{
    return GUINT32_FROM_LE(((SpiceDataHeader *)header->data)->size);
}

static uint32_t mini_header_get_msg_size(SpiceDataHeaderOpaque *header)
{
    return GUINT32_FROM_LE(((SpiceMiniDataHeader *)header->data)->size);
}

static uint16_t full_header_get_msg_type(SpiceDataHeaderOpaque *header)
{
    return GUINT16_FROM_LE(((SpiceDataHeader *)header->data)->type);
}

static uint16_t mini_header_get_msg_type(SpiceDataHeaderOpaque *header)
{
    return GUINT16_FROM_LE(((SpiceMiniDataHeader *)header->data)->type);
}

static void full_header_set_msg_type(SpiceDataHeaderOpaque *header, uint16_t type)
{
    ((SpiceDataHeader *)header->data)->type = GUINT16_TO_LE(type);
}

static void mini_header_set_msg_type(SpiceDataHeaderOpaque *header, uint16_t type)
{
    ((SpiceMiniDataHeader *)header->data)->type = GUINT16_TO_LE(type);
}

static void full_header_set_msg_size(SpiceDataHeaderOpaque *header, uint32_t size)
{
    ((SpiceDataHeader *)header->data)->size = GUINT32_TO_LE(size);
}

static void mini_header_set_msg_size(SpiceDataHeaderOpaque *header, uint32_t size)
{
    ((SpiceMiniDataHeader *)header->data)->size = GUINT32_TO_LE(size);
}

static void full_header_set_msg_serial(SpiceDataHeaderOpaque *header, uint64_t serial)
{
    ((SpiceDataHeader *)header->data)->serial = GUINT64_TO_LE(serial);
}

static void mini_header_set_msg_serial(SpiceDataHeaderOpaque *header, uint64_t serial)
{
    /* ignore serial, not supported by mini header */
}

static void full_header_set_msg_sub_list(SpiceDataHeaderOpaque *header, uint32_t sub_list)
{
    ((SpiceDataHeader *)header->data)->sub_list = GUINT32_TO_LE(sub_list);
}

static void mini_header_set_msg_sub_list(SpiceDataHeaderOpaque *header, uint32_t sub_list)
{
    spice_error("attempt to set header sub list on mini header");
}

bool RedChannelClient::init()
{
    char *local_error = nullptr;
    SpiceCoreInterfaceInternal *core;

    if (!priv->stream) {
        local_error =
            g_strdup_printf("Socket not available");
        goto cleanup;
    }

    if (!config_socket()) {
        local_error =
            g_strdup_printf("Unable to configure socket");
        goto cleanup;
    }

    core = priv->channel->get_core_interface();
    red_stream_set_core_interface(priv->stream, core);
    priv->stream->watch =
        core->watch_new(priv->stream->socket,
                        SPICE_WATCH_EVENT_READ,
                        red_channel_client_event,
                        this);

    if (red_stream_get_family(priv->stream) != AF_UNIX) {
        priv->latency_monitor.timer =
            core->timer_new(ping_timer, this);

        if (!priv->client->during_migrate_at_target()) {
            priv->start_ping_timer(PING_TEST_IDLE_NET_TIMEOUT_MS);
        }
        priv->latency_monitor.roundtrip = -1;
        priv->latency_monitor.timeout =
            priv->monitor_latency ? PING_TEST_TIMEOUT_MS : PING_TEST_LONG_TIMEOUT_MS;
    }

    priv->channel->add_client(this);
    if (!priv->client->add_channel(this, &local_error)) {
        priv->channel->remove_client(this);
    }

cleanup:
    if (local_error) {
        red_channel_warning(get_channel(),
                            "Failed to create channel client: %s",
                            local_error);
        g_free(local_error);
    }
    return local_error == nullptr;
}

void RedChannelClientPrivate::watch_update_mask(int event_mask)
{
    if (!stream->watch) {
        return;
    }

    if (block_read) {
        event_mask &= ~SPICE_WATCH_EVENT_READ;
    }

    red_watch_update_mask(stream->watch, event_mask);
}

void RedChannelClient::block_read()
{
    if (priv->block_read) {
        return;
    }
    priv->block_read = true;
    priv->watch_update_mask(SPICE_WATCH_EVENT_WRITE);
}

void RedChannelClient::unblock_read()
{
    if (!priv->block_read) {
        return;
    }
    priv->block_read = false;
    priv->watch_update_mask(SPICE_WATCH_EVENT_READ|SPICE_WATCH_EVENT_WRITE);
}

void RedChannelClientPrivate::seamless_migration_done()
{
    wait_migrate_data = FALSE;

    if (client->seamless_migration_done_for_channel()) {
        start_ping_timer(PING_TEST_IDLE_NET_TIMEOUT_MS);
        if (connectivity_monitor.timer) {
            red_timer_start(connectivity_monitor.timer,
                            connectivity_monitor.timeout);
        }
    }
}

void RedChannelClient::semi_seamless_migration_complete()
{
    priv->start_ping_timer(PING_TEST_IDLE_NET_TIMEOUT_MS);
}

bool RedChannelClient::is_waiting_for_migrate_data() const
{
    return priv->wait_migrate_data;
}

void RedChannelClient::migrate()
{
    priv->cancel_ping_timer();
    red_timer_remove(priv->latency_monitor.timer);
    priv->latency_monitor.timer = nullptr;

    red_timer_remove(priv->connectivity_monitor.timer);
    priv->connectivity_monitor.timer = nullptr;

    pipe_add_type(RED_PIPE_ITEM_TYPE_MIGRATE);
}

void RedChannelClient::shutdown()
{
    if (priv->stream && priv->stream->watch) {
        red_watch_remove(priv->stream->watch);
        priv->stream->watch = nullptr;
        ::shutdown(priv->stream->socket, SHUT_RDWR);
    }
}

void RedChannelClient::handle_outgoing()
{
    RedStream *stream = priv->stream;
    OutgoingMessageBuffer *buffer = &priv->outgoing;
    ssize_t n;

    if (!stream) {
        return;
    }

    if (buffer->size == 0) {
        buffer->size = priv->get_out_msg_size();
        if (!buffer->size) {  // nothing to be sent
            return;
        }
    }

    for (;;) {
        struct iovec vec[IOV_MAX];
        int vec_size =
            priv->prepare_out_msg(vec, G_N_ELEMENTS(vec), buffer->pos);
        n = red_stream_writev(stream, vec, vec_size);
        if (n == -1) {
            switch (errno) {
            case EAGAIN:
                priv->set_blocked();
                break;
            case EINTR:
                continue;
            case EPIPE:
                disconnect();
                break;
            default:
                red_channel_warning(get_channel(), "%s", strerror(errno));
                disconnect();
                break;
            }
            return;
        }
        buffer->pos += n;
        priv->data_sent(n);
        if (buffer->pos == buffer->size) { // finished writing data
            /* reset buffer before calling on_msg_done, since it
             * can trigger another call to RedChannelClient::handle_outgoing (when
             * switching from the urgent marshaller to the main one */
            buffer->pos = 0;
            buffer->size = 0;
            msg_sent();
            return;
        }
    }
}

/* return the number of bytes read. -1 in case of error */
static int red_peer_receive(RedStream *stream, uint8_t *buf, uint32_t size)
{
    uint8_t *pos = buf;
    while (size) {
        int now;
        /* if we don't have a watch it means socket has been shutdown
         * shutdown read doesn't work as accepted - receive may return data afterward.
         * check the flag before calling receive
         */
        if (!stream->watch) {
            return -1;
        }
        now = red_stream_read(stream, pos, size);
        if (now <= 0) {
            if (now == 0) {
                return -1;
            }
            spice_assert(now == -1);
            if (errno == EAGAIN) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EPIPE) {
                g_warning("%s", strerror(errno));
            }
            return -1;
        }
        size -= now;
        pos += now;
    }
    return pos - buf;
}

// TODO: this implementation, as opposed to the old implementation in red_worker,
// does many calls to red_peer_receive and through it cb_read, and thus avoids pointer
// arithmetic for the case where a single cb_read could return multiple messages. But
// this is suboptimal potentially. Profile and consider fixing.
void RedChannelClient::handle_incoming()
{
    RedStream *stream = priv->stream;
    IncomingMessageBuffer *buffer = &priv->incoming;
    int bytes_read;
    uint16_t msg_type;
    uint32_t msg_size;

    /* XXX: This needs further investigation as to the underlying cause, it happened
     * after spicec disconnect (but not with spice-gtk) repeatedly. */
    if (!stream) {
        return;
    }

    for (;;) {
        int ret_handle;
        uint8_t *parsed;
        size_t parsed_size;
        message_destructor_t parsed_free = nullptr;
        RedChannel *channel = get_channel();

        if (buffer->header_pos < buffer->header.header_size) {
            bytes_read = red_peer_receive(stream,
                                          buffer->header.data + buffer->header_pos,
                                          buffer->header.header_size - buffer->header_pos);
            if (bytes_read == -1) {
                disconnect();
                return;
            }
            priv->data_read(bytes_read);
            buffer->header_pos += bytes_read;

            if (buffer->header_pos != buffer->header.header_size) {
                return;
            }
        }

        msg_size = buffer->header.get_msg_size(&buffer->header);
        msg_type = buffer->header.get_msg_type(&buffer->header);
        if (buffer->msg_pos < msg_size) {
            if (!buffer->msg) {
                buffer->msg = alloc_recv_buf(msg_type, msg_size);
                if (buffer->msg == nullptr && priv->block_read) {
                    // if we are blocked by flow control just return, message will be read
                    // when data will be available
                    return;
                }
                if (buffer->msg == nullptr) {
                    red_channel_warning(channel, "ERROR: channel refused to allocate buffer.");
                    disconnect();
                    return;
                }
            }

            bytes_read = red_peer_receive(stream,
                                          buffer->msg + buffer->msg_pos,
                                          msg_size - buffer->msg_pos);
            if (bytes_read == -1) {
                release_recv_buf(msg_type, msg_size, buffer->msg);
                buffer->msg = nullptr;
                disconnect();
                return;
            }
            priv->data_read(bytes_read);
            buffer->msg_pos += bytes_read;
            if (buffer->msg_pos != msg_size) {
                return;
            }
        }

        parsed = get_channel()->parse(buffer->msg, msg_size,
                                      msg_type, &parsed_size, &parsed_free);
        if (parsed == nullptr) {
            red_channel_warning(channel, "failed to parse message type %d", msg_type);
            release_recv_buf(msg_type, msg_size, buffer->msg);
            buffer->msg = nullptr;
            disconnect();
            return;
        }
        ret_handle = handle_message(msg_type, parsed_size, parsed);
        if (parsed_free != nullptr) {
            parsed_free(parsed);
        }
        buffer->msg_pos = 0;
        release_recv_buf(msg_type, msg_size, buffer->msg);
        buffer->msg = nullptr;
        buffer->header_pos = 0;

        if (!ret_handle) {
            disconnect();
            return;
        }
    }
}

void RedChannelClient::receive()
{
    red::shared_ptr<RedChannelClient> hold_rcc(this);
    handle_incoming();
}

void RedChannelClient::send()
{
    red::shared_ptr<RedChannelClient> hold_rcc(this);
    handle_outgoing();
}

inline RedPipeItemPtr RedChannelClientPrivate::pipe_item_get()
{
    RedPipeItemPtr ret;

    if (send_data.blocked || waiting_for_ack() || pipe.empty()) {
        return ret;
    }
    ret = std::move(pipe.back());
    pipe.pop_back();
    return ret;
}

void RedChannelClient::push()
{
    if (priv->during_send) {
        return;
    }

    priv->during_send = TRUE;
    red::shared_ptr<RedChannelClient> hold_rcc(this);
    if (is_blocked()) {
        send();
    }

    if (!no_item_being_sent() && !is_blocked()) {
        priv->set_blocked();
        red_channel_warning(get_channel(),
                            "ERROR: an item waiting to be sent and not blocked");
    }

    while (auto pipe_item = priv->pipe_item_get()) {
        send_any_item(pipe_item.get());
    }
    /* prepare_pipe_add() will reenable WRITE events when the priv->pipe is empty
     * ack_zero_messages_window() will reenable WRITE events
     * if we were waiting for acks to be received
     * If we don't remove WRITE if we are waiting for ack we will be keep
     * notified that we can write and we then exit (see pipe_item_get) as we
     * are waiting for the ack consuming CPU in a tight loop
     */
    if ((no_item_being_sent() && priv->pipe.empty()) ||
        priv->waiting_for_ack()) {
        priv->watch_update_mask(SPICE_WATCH_EVENT_READ);

        /* channel has no pending data to send so now we can flush data in
         * order to avoid data stall into buffers in case of manual
         * flushing
         * We need to flush also in case of ack as it is possible
         * that for a long train of small messages the message that would
         * cause the client to send the ack is still in the queue
         */
        red_stream_flush(priv->stream);
    }
    priv->during_send = FALSE;
}

int RedChannelClient::get_roundtrip_ms() const
{
    if (priv->latency_monitor.roundtrip < 0) {
        return priv->latency_monitor.roundtrip;
    }
    return priv->latency_monitor.roundtrip / NSEC_PER_MILLISEC;
}

void RedChannelClient::init_outgoing_messages_window()
{
    priv->ack_data.messages_window = 0;
    push();
}

void RedChannelClientPrivate::handle_pong(SpiceMsgPing *ping)
{
    uint64_t now;

    /* ignoring unexpected pongs, or post-migration pongs for pings that
     * started just before migration */
    if (ping->id != latency_monitor.id) {
        spice_warning("ping-id (%u)!= pong-id %u",
                      latency_monitor.id, ping->id);
        return;
    }

    now = spice_get_monotonic_time_ns();

    if (latency_monitor.state == PING_STATE_WARMUP) {
        latency_monitor.state = PING_STATE_LATENCY;
        return;
    }
    if (latency_monitor.state != PING_STATE_LATENCY) {
        spice_warning("unexpected");
        return;
    }

    /* set TCP_NODELAY=0, in case we reverted it for the test*/
    if (!latency_monitor.tcp_nodelay) {
        red_stream_set_no_delay(stream, FALSE);
    }

    /*
     * The real network latency shouldn't change during the connection. However,
     *  the measurements can be bigger than the real roundtrip due to other
     *  threads or processes that are utilizing the network. We update the roundtrip
     *  measurement with the minimal value we encountered till now.
     */
    if (latency_monitor.roundtrip < 0 ||
        now - ping->timestamp < latency_monitor.roundtrip) {
        latency_monitor.roundtrip = now - ping->timestamp;
        spice_debug("update roundtrip %.2f(ms)", ((double)latency_monitor.roundtrip)/NSEC_PER_MILLISEC);
    }

    latency_monitor.last_pong_time = now;
    latency_monitor.state = PING_STATE_NONE;
    start_ping_timer(latency_monitor.timeout);
}

void RedChannelClient::handle_migrate_flush_mark()
{
}

// TODO: the whole migration is broken with multiple clients. What do we want to do?
// basically just
//  1) source send mark to all
//  2) source gets at various times the data (waits for all)
//  3) source migrates to target
//  4) target sends data to all
// So need to make all the handlers work with per channel/client data (what data exactly?)
void RedChannelClient::handle_migrate_data_early(uint32_t size, void *message)
{
    red_channel_debug(priv->channel, "rcc %p size %u", this, size);

    uint32_t flags = priv->channel->migration_flags();
    if (!(flags & SPICE_MIGRATE_NEED_DATA_TRANSFER)) {
        return;
    }
    if (!is_waiting_for_migrate_data()) {
        spice_channel_client_error(this, "unexpected");
        return;
    }
    uint64_t serial;
    if (handle_migrate_data_get_serial(size, message, serial)) {
        priv->set_message_serial(serial);
    }
    if (!handle_migrate_data(size, message)) {
        spice_channel_client_error(this, "handle_migrate_data failed");
        return;
    }
    priv->seamless_migration_done();
}

bool RedChannelClient::handle_message(uint16_t type, uint32_t size, void *message)
{
    switch (type) {
    case SPICE_MSGC_ACK_SYNC:
        priv->ack_data.client_generation = ((SpiceMsgcAckSync *) message)->generation;
        break;
    case SPICE_MSGC_ACK:
        if (priv->ack_data.client_generation == priv->ack_data.generation) {
            priv->ack_data.messages_window -= priv->ack_data.client_window;
            priv->watch_update_mask(SPICE_WATCH_EVENT_READ|SPICE_WATCH_EVENT_WRITE);
            push();
        }
        break;
    case SPICE_MSGC_DISCONNECTING:
        break;
    case SPICE_MSGC_MIGRATE_FLUSH_MARK:
        if (!priv->wait_migrate_flush_mark) {
            spice_error("unexpected flush mark");
            return FALSE;
        }
        handle_migrate_flush_mark();
        priv->wait_migrate_flush_mark = FALSE;
        break;
    case SPICE_MSGC_MIGRATE_DATA:
        handle_migrate_data_early(size, message);
        break;
    case SPICE_MSGC_PONG:
        priv->handle_pong((SpiceMsgPing*) message);
        break;
    default:
        red_channel_warning(get_channel(), "invalid message type %u",
                            type);
        return FALSE;
    }
    return TRUE;
}

void RedChannelClient::init_send_data(uint16_t msg_type)
{
    spice_assert(no_item_being_sent());
    spice_assert(msg_type != 0);
    priv->send_data.header.set_msg_type(&priv->send_data.header, msg_type);
}

void RedChannelClient::begin_send_message()
{
    SpiceMarshaller *m = priv->send_data.marshaller;

    // TODO - better check: type in channel_allowed_types. Better: type in channel_allowed_types(channel_state)
    if (priv->send_data.header.get_msg_type(&priv->send_data.header) == 0) {
        red_channel_warning(get_channel(), "BUG: header->type == 0");
        return;
    }

    stat_inc_counter(priv->out_messages, 1);

    /* canceling the latency test timer till the nework is idle */
    priv->cancel_ping_timer();

    spice_marshaller_flush(m);
    priv->send_data.size = spice_marshaller_get_total_size(m);
    priv->send_data.header.set_msg_size(&priv->send_data.header,
                                             priv->send_data.size -
                                             priv->send_data.header.header_size);
    priv->send_data.header.set_msg_serial(&priv->send_data.header,
                                               ++priv->send_data.last_sent_serial);
    priv->ack_data.messages_window++;
    priv->send_data.header.data = nullptr; /* avoid writing to this until we have a new message */
    send();
}

SpiceMarshaller *RedChannelClient::switch_to_urgent_sender()
{
    spice_assert(no_item_being_sent());
    spice_assert(priv->send_data.header.data != nullptr);
    priv->send_data.main.header_data = priv->send_data.header.data;

    priv->send_data.marshaller = priv->send_data.urgent.marshaller;
    priv->reset_send_data();
    return priv->send_data.marshaller;
}

uint64_t RedChannelClient::get_message_serial() const
{
    return priv->send_data.last_sent_serial + 1;
}

inline void RedChannelClientPrivate::set_message_serial(uint64_t serial)
{
    send_data.last_sent_serial = serial - 1;
}

inline bool RedChannelClient::prepare_pipe_add(RedPipeItem *item)
{
    spice_assert(item);
    if (SPICE_UNLIKELY(!is_connected())) {
        spice_debug("rcc is disconnected %p", this);
        return false;
    }
    if (priv->pipe.empty()) {
        priv->watch_update_mask(SPICE_WATCH_EVENT_READ | SPICE_WATCH_EVENT_WRITE);
    }
    return true;
}

void RedChannelClient::pipe_add(RedPipeItemPtr&& item)
{
    if (!prepare_pipe_add(item.get())) {
        return;
    }
    priv->pipe.push_front(std::move(item));
}

void RedChannelClient::pipe_add_push(RedPipeItemPtr&& item)
{
    pipe_add(std::move(item));
    push();
}

void RedChannelClient::pipe_add_after_pos(RedPipeItemPtr&& item,
                                          Pipe::iterator pipe_item_pos)
{
    spice_assert(pipe_item_pos != priv->pipe.end());
    if (!prepare_pipe_add(item.get())) {
        return;
    }

    ++pipe_item_pos;
    priv->pipe.insert(pipe_item_pos, std::move(item));
}

void
RedChannelClient::pipe_add_before_pos(RedPipeItemPtr&& item, Pipe::iterator pipe_item_pos)
{
    spice_assert(pipe_item_pos != priv->pipe.end());
    if (!prepare_pipe_add(item.get())) {
        return;
    }

    priv->pipe.insert(pipe_item_pos, std::move(item));
}

void RedChannelClient::pipe_add_after(RedPipeItemPtr&& item, RedPipeItem *pos)
{
    spice_assert(pos);
    auto prev = find_pipe_item(priv->pipe, pos);
    g_return_if_fail(prev != priv->pipe.end());

    pipe_add_after_pos(std::move(item), prev);
}

bool RedChannelClient::pipe_item_is_linked(RedPipeItem *item) const
{
    return find_pipe_item(priv->pipe, item) != priv->pipe.end();
}

void RedChannelClient::pipe_add_tail(RedPipeItemPtr&& item)
{
    if (!prepare_pipe_add(item.get())) {
        return;
    }
    priv->pipe.push_back(std::move(item));
}

void RedChannelClient::pipe_add_type(int pipe_item_type)
{
    auto item = red::make_shared<RedPipeItem>(pipe_item_type);

    pipe_add(std::move(item));
}

RedPipeItemPtr RedChannelClient::new_empty_msg(int msg_type)
{
    auto item = red::make_shared<RedEmptyMsgPipeItem>();

    item->msg = msg_type;
    return item;
}

void RedChannelClient::pipe_add_empty_msg(int msg_type)
{
    pipe_add(new_empty_msg(msg_type));
}

bool RedChannelClient::pipe_is_empty() const
{
    return priv->pipe.empty();
}

uint32_t RedChannelClient::get_pipe_size() const
{
    return priv->pipe.size();
}

RedChannelClient::Pipe& RedChannelClient::get_pipe()
{
    return priv->pipe;
}

bool RedChannelClient::is_mini_header() const
{
    return priv->is_mini_header;
}

bool RedChannelClient::is_connected() const
{
    return g_list_find(priv->channel->get_clients(), this) != nullptr;
}

void RedChannelClientPrivate::clear_sent_item()
{
    send_data.blocked = FALSE;
    send_data.size = 0;
    spice_marshaller_reset(send_data.marshaller);
}

// TODO: again - what is the context exactly? this happens in channel disconnect. but our
// current red_channel_shutdown also closes the socket - is there a socket to close?
// are we reading from an fd here? arghh
void RedChannelClientPrivate::pipe_clear()
{
    clear_sent_item();
    pipe.clear();
}

void RedChannelClient::ack_zero_messages_window()
{
    priv->watch_update_mask(SPICE_WATCH_EVENT_READ|SPICE_WATCH_EVENT_WRITE);
    priv->ack_data.messages_window = 0;
}

void RedChannelClient::ack_set_client_window(int client_window)
{
    priv->ack_data.client_window = client_window;
}

void RedChannelClient::push_set_ack()
{
    pipe_add_type(RED_PIPE_ITEM_TYPE_SET_ACK);
}

void RedChannelClient::disconnect()
{
    auto channel = priv->channel;

    if (!is_connected()) {
        return;
    }
    priv->pipe_clear();

    shutdown();

    red_timer_remove(priv->latency_monitor.timer);
    priv->latency_monitor.timer = nullptr;

    red_timer_remove(priv->connectivity_monitor.timer);
    priv->connectivity_monitor.timer = nullptr;

    channel->remove_client(this);
    on_disconnect();
    // remove client from RedClient
    // NOTE this may trigger the free of the object, if we are in a watch/timer
    // we should make sure we keep a reference
    get_client()->remove_channel(this);
}

bool RedChannelClient::is_blocked() const
{
    return priv->send_data.blocked;
}

int RedChannelClient::send_message_pending()
{
    return priv->send_data.header.get_msg_type(&priv->send_data.header) != 0;
}

SpiceMarshaller *RedChannelClient::get_marshaller()
{
    return priv->send_data.marshaller;
}

RedStream *RedChannelClient::get_stream()
{
    return priv->stream;
}

RedClient *RedChannelClient::get_client()
{
    return priv->client;
}

void RedChannelClient::set_header_sub_list(uint32_t sub_list)
{
    priv->send_data.header.set_msg_sub_list(&priv->send_data.header, sub_list);
}

/* TODO: more evil sync stuff. anything with the word wait in it's name. */
bool RedChannelClient::wait_pipe_item_sent(Pipe::iterator item_pos, int64_t timeout)
{
    uint64_t end_time;

    spice_debug("trace");

    if (timeout != -1) {
        end_time = spice_get_monotonic_time_ns() + timeout;
    } else {
        end_time = UINT64_MAX;
    }

    auto mark_item = red::make_shared<MarkerPipeItem>();

    mark_item->item_sent = false;
    pipe_add_before_pos(RedPipeItemPtr(mark_item), item_pos);

    for (;;) {
        receive();
        push();
        if (mark_item->item_sent ||
            (timeout != -1 && spice_get_monotonic_time_ns() >= end_time)) {
            break;
        }
        usleep(CHANNEL_BLOCKED_SLEEP_DURATION);
    }

    if (!mark_item->item_sent) {
        // still on the queue
        spice_warning("timeout");
    }
    return mark_item->item_sent;
}

bool RedChannelClient::wait_outgoing_item(int64_t timeout)
{
    uint64_t end_time;
    int blocked;

    if (!is_blocked()) {
        return TRUE;
    }
    if (timeout != -1) {
        end_time = spice_get_monotonic_time_ns() + timeout;
    } else {
        end_time = UINT64_MAX;
    }
    spice_debug("blocked");

    do {
        usleep(CHANNEL_BLOCKED_SLEEP_DURATION);
        receive();
        send();
    } while ((blocked = is_blocked()) &&
             (timeout == -1 || spice_get_monotonic_time_ns() < end_time));

    if (blocked) {
        spice_warning("timeout");
        return FALSE;
    }
    spice_assert(no_item_being_sent());
    return TRUE;
}

bool RedChannelClient::no_item_being_sent() const
{
    return priv->send_data.size == 0;
}

void RedChannelClient::pipe_remove_and_release(RedPipeItem *item)
{
    priv->pipe_remove(item);
}

/* client mutex should be locked before this call */
bool RedChannelClient::set_migration_seamless()
{
    bool ret = false;
    uint32_t flags;

    flags = priv->channel->migration_flags();
    if (flags & SPICE_MIGRATE_NEED_DATA_TRANSFER) {
        priv->wait_migrate_data = TRUE;
        ret = true;
    }
    red_channel_debug(priv->channel, "rcc %p wait data %d", this,
                      priv->wait_migrate_data);

    return ret;
}
