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


    Author:
        yhalperi@redhat.com
*/

#ifndef RED_CHANNEL_H_
#define RED_CHANNEL_H_

#include <pthread.h>
#include <limits.h>
#include <common/marshaller.h>
#include <common/demarshallers.h>

#include "spice-wrapped.h"
#include "red-common.h"
#include "red-stream.h"
#include "stat.h"
#include "red-pipe-item.h"
#include "red-channel-capabilities.h"
#include "utils.hpp"

#include "push-visibility.h"
class RedChannel;
struct RedChannelPrivate;
struct RedChannelClient;
struct RedClient;
struct MainChannelClient;
struct Dispatcher;

static inline gboolean test_capability(const uint32_t *caps, int num_caps, uint32_t cap)
{
    return VD_AGENT_HAS_CAPABILITY(caps, num_caps, cap);
}

#define FOREACH_CLIENT(_channel, _data) \
    GLIST_FOREACH(_channel->get_clients(), RedChannelClient, _data)

/* Red Channel interface */

struct RedChannel: public red::shared_ptr_counted
{
    typedef enum {
        FlagNone = 0,
        MigrateNeedFlush = SPICE_MIGRATE_NEED_FLUSH,
        MigrateNeedDataTransfer = SPICE_MIGRATE_NEED_DATA_TRANSFER,
        HandleAcks = 8,
        MigrateAll = MigrateNeedFlush|MigrateNeedDataTransfer,
    } CreationFlags;

    RedChannel(RedsState *reds, uint32_t type, uint32_t id, CreationFlags flags=FlagNone,
               SpiceCoreInterfaceInternal *core=nullptr, Dispatcher *dispatcher=nullptr);
    virtual ~RedChannel();

    uint32_t id() const;
    uint32_t type() const;
    uint32_t migration_flags() const;
    bool handle_acks() const;

    virtual void on_connect(RedClient *client, RedStream *stream, int migration,
                            RedChannelCapabilities *caps) = 0;

    uint8_t *parse(uint8_t *message, size_t message_size,
                   uint16_t message_type,
                   size_t *size_out, message_destructor_t *free_message) const;

    const char *get_name() const;

    void add_client(RedChannelClient *rcc);
    void remove_client(RedChannelClient *rcc);

    void init_stat_node(const RedStatNode *parent, const char *name);

    // caps are freed when the channel is destroyed
    void set_common_cap(uint32_t cap);
    void set_cap(uint32_t cap);

    int is_connected();

    /* seamless migration is supported for only one client. This routine
     * checks if the only channel client associated with channel is
     * waiting for migration data */
    bool is_waiting_for_migrate_data();

    /*
     * the disconnect callback is called from the channel's thread,
     * i.e., for display channels - red worker thread, for all the other - from the main thread.
     * RedChannel::destroy can be called only from channel thread.
     */

    void destroy();

    /* return true if all the channel clients support the cap */
    bool test_remote_cap(uint32_t cap);

    // helper to push a new item to all channels
    typedef RedPipeItemPtr (*new_pipe_item_t)(RedChannelClient *rcc, void *data, int num);
    int pipes_new_add(new_pipe_item_t creator, void *data);

    void pipes_add_type(int pipe_item_type);

    void pipes_add_empty_msg(int msg_type);

    /* Add an item to all the clients connected.
     * The same item is shared between all clients.
     * Function will take ownership of the item.
     */
    void pipes_add(RedPipeItemPtr&& item);

    /* return TRUE if all of the connected clients to this channel are blocked */
    bool all_blocked();

    // TODO: unstaticed for display/cursor channels. they do some specific pushes not through
    // adding elements or on events. but not sure if this is actually required (only result
    // should be that they ""try"" a little harder, but if the event system is correct it
    // should not make any difference.
    void push();
    // Again, used in various places outside of event handler context (or in other event handler
    // contexts):
    //  flush_display_commands/flush_cursor_commands
    //  display_channel_wait_for_init
    //  red_wait_outgoing_item
    //  red_wait_pipe_item_sent
    //  handle_channel_events - this is the only one that was used before, and was in red-channel.c
    void receive();
    // For red_worker
    void send();
    // For red_worker
    void disconnect();
    void connect(RedClient *client, RedStream *stream, int migration,
                 RedChannelCapabilities *caps);

    /* return the sum of all the rcc pipe size */
    uint32_t max_pipe_size();
    /* return the max size of all the rcc pipe */
    uint32_t sum_pipes_size();

    GList *get_clients();
    guint get_n_clients();
    struct RedsState* get_server();
    SpiceCoreInterfaceInternal* get_core_interface();

    /* channel callback function */
    void reset_thread_id();
    const RedStatNode *get_stat_node();

    const RedChannelCapabilities* get_local_capabilities();

    /*
     * blocking functions.
     *
     * timeout is in nano sec. -1 for no timeout.
     *
     * This method tries for up to @timeout nanoseconds to send all the
     * items which are currently queued. If the timeout elapses,
     * the RedChannelClient which are too slow (those which still have pending
     * items) will be disconnected.
     *
     * Return: TRUE if waiting succeeded. FALSE if timeout expired.
     */

    bool wait_all_sent(int64_t timeout);

    /* wrappers for client callbacks */
    void migrate_client(RedChannelClient *rcc);
    void disconnect_client(RedChannelClient *rcc);

    red::unique_link<RedChannelPrivate> priv;
};

inline RedChannel::CreationFlags operator|(RedChannel::CreationFlags a, RedChannel::CreationFlags b)
{
    return (RedChannel::CreationFlags) ((int)a|(int)b);
}

#define CHANNEL_BLOCKED_SLEEP_DURATION 10000 //micro

#define red_channel_log_generic(log_cb, channel, format, ...)                            \
    do {                                                                                 \
        auto channel_ = (channel);                                                       \
        uint32_t id_ = channel_->id();                                                   \
        log_cb("%s:%u (%p): " format, channel_->get_name(),                              \
                        id_, &*channel_, ## __VA_ARGS__);                                \
    } while (0)

#define red_channel_warning(...) red_channel_log_generic(g_warning, __VA_ARGS__)
#define red_channel_message(...) red_channel_log_generic(g_message, __VA_ARGS__)
#define red_channel_debug(...) red_channel_log_generic(g_debug, __VA_ARGS__)

#include "pop-visibility.h"

#endif /* RED_CHANNEL_H_ */
