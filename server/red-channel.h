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
#include <glib-object.h>
#include <common/ring.h>
#include <common/marshaller.h>
#include <common/demarshallers.h>

#include "spice.h"
#include "red-common.h"
#include "red-stream.h"
#include "stat.h"
#include "red-pipe-item.h"
#include "red-channel-capabilities.h"

G_BEGIN_DECLS

SPICE_DECLARE_TYPE(RedChannel, red_channel, CHANNEL);
#define RED_TYPE_CHANNEL red_channel_get_type()

typedef struct RedChannelClient RedChannelClient;
typedef struct RedClient RedClient;
typedef struct MainChannelClient MainChannelClient;

typedef bool (*channel_handle_message_proc)(RedChannelClient *rcc, uint16_t type,
                                            uint32_t size, void *msg);
typedef void (*channel_send_pipe_item_proc)(RedChannelClient *rcc, RedPipeItem *item);

typedef bool (*channel_handle_migrate_flush_mark_proc)(RedChannelClient *base);
typedef bool (*channel_handle_migrate_data_proc)(RedChannelClient *base,
                                                 uint32_t size, void *message);
typedef uint64_t (*channel_handle_migrate_data_get_serial_proc)(RedChannelClient *base,
                                            uint32_t size, void *message);


typedef void (*channel_client_connect_proc)(RedChannel *channel, RedClient *client, RedStream *stream,
                                            int migration, RedChannelCapabilities *caps);
typedef void (*channel_client_disconnect_proc)(RedChannelClient *base);
typedef void (*channel_client_migrate_proc)(RedChannelClient *base);


static inline gboolean test_capability(const uint32_t *caps, int num_caps, uint32_t cap)
{
    return VD_AGENT_HAS_CAPABILITY(caps, num_caps, cap);
}

struct RedChannel
{
    GObject parent;

    RedChannelPrivate *priv;
};

struct RedChannelClass
{
    GObjectClass parent_class;

    /* subclasses must implement handle_message() and optionally parser().
     * If parser() is implemented, then handle_message() will get passed the
     * parsed message as its 'msg' argument, otherwise it will be passed
     * the raw data. In both cases, the 'size' argument is the length of 'msg'
     * in bytes
     */
    spice_parse_channel_func_t parser;
    channel_handle_message_proc handle_message;

    // TODO: add ASSERTS for thread_id  in client and channel calls
    /*
     * callbacks that are triggered from channel client stream events.
     * They are called from the thread that listen to the stream events.
     */
    channel_send_pipe_item_proc send_item;
    channel_handle_migrate_flush_mark_proc handle_migrate_flush_mark;
    channel_handle_migrate_data_proc handle_migrate_data;
    channel_handle_migrate_data_get_serial_proc handle_migrate_data_get_serial;

    /*
     * callbacks that are triggered from client events.
     * They should be called from the thread that handles the RedClient
     */
    channel_client_connect_proc connect;
    channel_client_disconnect_proc disconnect;
    channel_client_migrate_proc migrate;
};

#define FOREACH_CLIENT(_channel, _data) \
    GLIST_FOREACH((_channel ? red_channel_get_clients(RED_CHANNEL(_channel)) : NULL), \
                  RedChannelClient, _data)

/* Red Channel interface */

const char *red_channel_get_name(RedChannel *channel);

void red_channel_add_client(RedChannel *channel, RedChannelClient *rcc);
void red_channel_remove_client(RedChannel *channel, RedChannelClient *rcc);

void red_channel_init_stat_node(RedChannel *channel, const RedStatNode *parent, const char *name);

// caps are freed when the channel is destroyed
void red_channel_set_common_cap(RedChannel *channel, uint32_t cap);
void red_channel_set_cap(RedChannel *channel, uint32_t cap);

int red_channel_is_connected(RedChannel *channel);

/* seamless migration is supported for only one client. This routine
 * checks if the only channel client associated with channel is
 * waiting for migration data */
bool red_channel_is_waiting_for_migrate_data(RedChannel *channel);

/*
 * the disconnect callback is called from the channel's thread,
 * i.e., for display channels - red worker thread, for all the other - from the main thread.
 * RedClient is managed from the main thread. red_channel_client_destroy can be called only
 * from red_client_destroy.
 */

void red_channel_destroy(RedChannel *channel);

/* return true if all the channel clients support the cap */
bool red_channel_test_remote_cap(RedChannel *channel, uint32_t cap);

// helper to push a new item to all channels
typedef RedPipeItem *(*new_pipe_item_t)(RedChannelClient *rcc, void *data, int num);
int red_channel_pipes_new_add(RedChannel *channel, new_pipe_item_t creator, void *data);

void red_channel_pipes_add_type(RedChannel *channel, int pipe_item_type);

void red_channel_pipes_add_empty_msg(RedChannel *channel, int msg_type);

/* Add an item to all the clients connected.
 * The same item is shared between all clients.
 * Function will take ownership of the item.
 */
void red_channel_pipes_add(RedChannel *channel, RedPipeItem *item);

/* return TRUE if all of the connected clients to this channel are blocked */
bool red_channel_all_blocked(RedChannel *channel);

// TODO: unstaticed for display/cursor channels. they do some specific pushes not through
// adding elements or on events. but not sure if this is actually required (only result
// should be that they ""try"" a little harder, but if the event system is correct it
// should not make any difference.
void red_channel_push(RedChannel *channel);
// Again, used in various places outside of event handler context (or in other event handler
// contexts):
//  flush_display_commands/flush_cursor_commands
//  display_channel_wait_for_init
//  red_wait_outgoing_item
//  red_wait_pipe_item_sent
//  handle_channel_events - this is the only one that was used before, and was in red-channel.c
void red_channel_receive(RedChannel *channel);
// For red_worker
void red_channel_send(RedChannel *channel);
// For red_worker
void red_channel_disconnect(RedChannel *channel);
void red_channel_connect(RedChannel *channel, RedClient *client,
                         RedStream *stream, int migration,
                         RedChannelCapabilities *caps);

/* return the sum of all the rcc pipe size */
uint32_t red_channel_max_pipe_size(RedChannel *channel);
/* return the max size of all the rcc pipe */
uint32_t red_channel_sum_pipes_size(RedChannel *channel);

GList *red_channel_get_clients(RedChannel *channel);
guint red_channel_get_n_clients(RedChannel *channel);
struct RedsState* red_channel_get_server(RedChannel *channel);
SpiceCoreInterfaceInternal* red_channel_get_core_interface(RedChannel *channel);

/* channel callback function */
void red_channel_send_item(RedChannel *self, RedChannelClient *rcc, RedPipeItem *item);
void red_channel_reset_thread_id(RedChannel *self);
const RedStatNode *red_channel_get_stat_node(RedChannel *channel);

const RedChannelCapabilities* red_channel_get_local_capabilities(RedChannel *self);

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

bool red_channel_wait_all_sent(RedChannel *channel,
                               int64_t timeout);

/* wrappers for client callbacks */
void red_channel_migrate_client(RedChannel *channel, RedChannelClient *rcc);
void red_channel_disconnect_client(RedChannel *channel, RedChannelClient *rcc);

#define CHANNEL_BLOCKED_SLEEP_DURATION 10000 //micro

#define red_channel_log_generic(log_cb, channel, format, ...)                            \
    do {                                                                                 \
        uint32_t id_;                                                                    \
        RedChannel *channel_ = (channel);                                                \
        g_object_get(channel_, "id", &id_, NULL);                                        \
        log_cb("%s:%u (%p): " format, red_channel_get_name(channel_),                    \
                        id_, channel_, ## __VA_ARGS__);                                  \
    } while (0)

#define red_channel_warning(channel, format, ...)                                        \
        red_channel_log_generic(g_warning, channel, format, ## __VA_ARGS__);

#define red_channel_message(channel, format, ...)                                        \
        red_channel_log_generic(g_message, channel, format, ## __VA_ARGS__);

#define red_channel_debug(channel, format, ...)                                          \
        red_channel_log_generic(g_debug, channel, format, ## __VA_ARGS__);

G_END_DECLS

#endif /* RED_CHANNEL_H_ */
