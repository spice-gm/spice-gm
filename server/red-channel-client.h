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

#ifndef RED_CHANNEL_CLIENT_H_
#define RED_CHANNEL_CLIENT_H_

#include <list>
#include <common/marshaller.h>

#include "red-pipe-item.h"
#include "red-stream.h"
#include "red-channel.h"
#include "utils.hpp"
#include "safe-list.hpp"

#include "push-visibility.h"

struct RedChannelClientPrivate;

class RedChannelClient: public red::shared_ptr_counted
{
    // This is made protected to avoid allocation on stack conflicting with
    // reference counting
protected:
    virtual ~RedChannelClient();

public:
    RedChannelClient(RedChannel *channel,
                     RedClient *client,
                     RedStream *stream,
                     RedChannelCapabilities *caps,
                     bool monitor_latency=false);
    virtual bool init();

    bool is_connected() const;
    bool is_waiting_for_migrate_data() const;
    bool test_remote_cap(uint32_t cap) const;
    /* shutdown is the only safe thing to do out of the client/channel
     * thread. It will not touch the rings, just shutdown the socket.
     * It should be followed by some way to guarantee a disconnection. */
    void shutdown();
    /* when preparing send_data: should call init and then use marshaller */
    void init_send_data(uint16_t msg_type);

    uint64_t get_message_serial() const;

    /* When sending a msg. Should first call begin_send_message.
     * It will first send the pending urgent data, if there is any, and then
     * the rest of the data.
     */
    void begin_send_message();

    /*
     * Stores the current send data, and switches to urgent send data.
     * When it begins the actual send, it will send first the urgent data
     * and afterward the rest of the data.
     * Should be called only if during the marshalling of on message,
     * the need to send another message, before, rises.
     * Important: the serial of the non-urgent sent data, will be succeeded.
     * return: the urgent send data marshaller
     */
    SpiceMarshaller *switch_to_urgent_sender();

    /* returns -1 if we don't have an estimation */
    int get_roundtrip_ms() const;

protected:
    /* Checks periodically if the connection is still alive */
    void start_connectivity_monitoring(uint32_t timeout_ms);

public:
    typedef std::list<RedPipeItemPtr, red::Mallocator<RedPipeItemPtr>> Pipe;

    void pipe_add_push(RedPipeItemPtr&& item);
    void pipe_add(RedPipeItemPtr&& item);
    void pipe_add_after(RedPipeItemPtr&& item, RedPipeItem *pos);
    void pipe_add_after_pos(RedPipeItemPtr&& item,
                            RedChannelClient::Pipe::iterator pos);
    bool pipe_item_is_linked(RedPipeItem *item) const;
    void pipe_remove_and_release(RedPipeItem *item);
    void pipe_add_tail(RedPipeItemPtr&& item);
    /* for types that use this routine -> the pipe item should be freed */
    void pipe_add_type(int pipe_item_type);
    static RedPipeItemPtr new_empty_msg(int msg_type);
    void pipe_add_empty_msg(int msg_type);
    bool pipe_is_empty() const;
    uint32_t get_pipe_size() const;
    Pipe& get_pipe();
    bool is_mini_header() const;

    void ack_zero_messages_window();
    void ack_set_client_window(int client_window);
    void push_set_ack();

    bool is_blocked() const;

    /* helper for channels that have complex logic that can possibly ready a send */
    int send_message_pending();

    bool no_item_being_sent() const;
    void push();
    void receive();
    void send();
    virtual void disconnect();

    /* Note: the valid times to call red_channel_get_marshaller are just during send_item callback. */
    SpiceMarshaller *get_marshaller();
    RedStream *get_stream();
    RedClient *get_client();

    /* Note that the header is valid only between reset_send_data and
     * begin_send_message.*/
    void set_header_sub_list(uint32_t sub_list);

    /*
     * blocking functions.
     *
     * timeout is in nano sec. -1 for no timeout.
     *
     * Return: TRUE if waiting succeeded. FALSE if timeout expired.
     */

    bool wait_pipe_item_sent(RedChannelClient::Pipe::iterator item_pos, int64_t timeout);
    bool wait_outgoing_item(int64_t timeout);

    RedChannel* get_channel();

    void semi_seamless_migration_complete();

    bool set_migration_seamless();

    /* allow to block or unblock reading */
    void block_read();
    void unblock_read();

    // callback from client
    virtual void migrate();

protected:
    bool test_remote_common_cap(uint32_t cap) const;
    void init_outgoing_messages_window();

    /* handles general channel msgs from the client */
    virtual bool handle_message(uint16_t type, uint32_t size, void *message);

    /* configure socket connected to the client */
    virtual bool config_socket() { return true; }
    virtual uint8_t *alloc_recv_buf(uint16_t type, uint32_t size)=0;
    virtual void release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg)=0;

    virtual void on_disconnect() {};

    // TODO: add ASSERTS for thread_id  in client and channel calls
    /*
     * callbacks that are triggered from channel client stream events.
     * They are called from the thread that listen to the stream events.
     */
    virtual void send_item(RedPipeItem *item) {};

    virtual bool handle_migrate_data(uint32_t size, void *message) { return false; }
    virtual bool handle_migrate_data_get_serial(uint32_t size, void *message, uint64_t &serial)
    {
        return false;
    }

    /* Private functions */
private:
    void send_any_item(RedPipeItem *item);
    void handle_outgoing();
    void handle_incoming();
    virtual void handle_migrate_flush_mark();
    void handle_migrate_data_early(uint32_t size, void *message);
    inline bool prepare_pipe_add(RedPipeItem *item);
    void pipe_add_before_pos(RedPipeItemPtr&& item, RedChannelClient::Pipe::iterator pipe_item_pos);
    void send_set_ack();
    void send_migrate();
    void send_empty_msg(RedPipeItem *base);
    void msg_sent();
    static void ping_timer(RedChannelClient *rcc);
    static void connectivity_timer(RedChannelClient *rcc);
    void send_ping();
    void push_ping();

    /* Private data */
private:
    red::unique_link<RedChannelClientPrivate> priv;
};

/* Messages handled by RedChannel
 * SET_ACK - sent to client on channel connection
 * Note that the numbers don't have to correspond to spice message types,
 * but we keep the 100 first allocated for base channel approach.
 * */
enum {
    RED_PIPE_ITEM_TYPE_SET_ACK=1,
    RED_PIPE_ITEM_TYPE_MIGRATE,
    RED_PIPE_ITEM_TYPE_EMPTY_MSG,
    RED_PIPE_ITEM_TYPE_PING,
    RED_PIPE_ITEM_TYPE_MARKER,

    RED_PIPE_ITEM_TYPE_CHANNEL_BASE=101,
};

#include "pop-visibility.h"

#endif /* RED_CHANNEL_CLIENT_H_ */
