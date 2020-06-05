/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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

#ifndef CHAR_DEVICE_H_
#define CHAR_DEVICE_H_

#include "spice-wrapped.h"
#include "red-channel.h"
#include "migration-protocol.h"
#include "utils.hpp"

#include "push-visibility.h"

struct RedCharDevice;
struct RedCharDevicePrivate;
struct RedCharDeviceClientOpaque;

/*
 * Shared code for char devices, mainly for flow control.
 *
 * How to use the api:
 * ==================
 * device attached: create new object instantiating a RedCharDevice child class
 * device detached: unreference/RedCharDevice::reset
 *
 * client connected and associated with a device: RedCharDevice::client_add
 * client disconnected: RedCharDevice::client_remove
 *
 * Writing to the device
 * ---------------------
 * Write the data into RedCharDeviceWriteBuffer:
 * call RedCharDevice::write_buffer_get_client/RedCharDevice::write_buffer_get_server
 * in order to get an appropriate buffer.
 * call RedCharDevice::write_buffer_add in order to push the buffer to the write queue.
 * If you choose not to push the buffer to the device, call
 * RedCharDevice::write_buffer_release
 *
 * reading from the device
 * -----------------------
 *  The callback read_one_msg_from_device (see below) should be implemented
 *  (using sif->read).
 *  When the device is ready, this callback is called, and is expected to
 *  return one message which is addressed to the client, or NULL if the read
 *  hasn't completed.
 *
 * calls triggered from the device (qemu):
 * --------------------------------------
 * RedCharDevice::start
 * RedCharDevice::stop
 * RedCharDevice::wakeup (for reading from the device)
 */
/* refcounting is used to protect the char_dev from being deallocated in
 * case object is unreferenced during a callback, and we might still access
 * the char_dev afterwards.
 */


/*
 * Note about multiple-clients:
 * Multiclients are currently not supported in any of the character devices:
 * spicevmc does not allow more than one client (and at least for usb, it should stay this way).
 * smartcard code is not compatible with more than one reader.
 * The server and guest agent code doesn't distinguish messages from different clients.
 * In addition, its current flow control code (e.g., tokens handling) is wrong and doesn't
 * take into account the different clients.
 *
 * Nonetheless, the following code introduces some support for multiple-clients:
 * We track the number of tokens for all the clients, and we read from the device
 * if one of the clients have enough tokens. For the clients that don't have tokens,
 * we queue the messages, till they receive tokens, or till a timeout.
 *
 * TODO:
 * At least for the agent, not all the messages from the device will be directed to all
 * the clients (e.g., copy from guest to a specific client). Thus, support for
 * client-specific-messages should be added.
 * In addition, we should have support for clients that are being connected
 * in the middle of a message transfer from the agent to the clients.
 *
 * */

/* buffer that is used for writing to the device */
struct RedCharDeviceWriteBufferPrivate;
struct RedCharDeviceWriteBuffer {
    uint32_t buf_size;
    uint32_t buf_used;

    RedCharDeviceWriteBufferPrivate *priv;
    uint8_t buf[0];
};


class RedCharDevice: public red::shared_ptr_counted_weak
{
public:
    RedCharDevice(RedsState *reds, SpiceCharDeviceInstance *sin,
                  uint64_t client_tokens_interval, uint64_t num_self_tokens);
    ~RedCharDevice();

    void reset_dev_instance(SpiceCharDeviceInstance *sin);
    /* only one client is supported */
    void migrate_data_marshall(SpiceMarshaller *m);
    static void migrate_data_marshall_empty(SpiceMarshaller *m);

    bool restore(SpiceMigrateDataCharDevice *mig_data);

    /*
     * Resets write/read queues, and moves that state to being stopped.
     * This routine is a workaround for a bad tokens management in the vdagent
     * protocol:
     *  The client tokens' are set only once, when the main channel is initialized.
     *  Instead, it would have been more appropriate to reset them upon AGENT_CONNECT.
     *  The client tokens are tracked as part of the RedCharDeviceClient. Thus,
     *  in order to be backward compatible with the client, we need to track the tokens
     *  event when the agent is detached. We don't destroy the char_device state, and
     *  instead we just reset it.
     *  In addition, there is a misshandling of AGENT_TOKENS message in spice-gtk: it
     *  overrides the amount of tokens, instead of adding the given amount.
     */
    void reset();

    /* max_send_queue_size = how many messages we can read from the device and enqueue for this client,
     * when we have tokens for other clients and no tokens for this one */
    bool client_add(RedCharDeviceClientOpaque *client, int do_flow_control,
                    uint32_t max_send_queue_size, uint32_t num_client_tokens,
                    uint32_t num_send_tokens, int wait_for_migrate_data);

    void client_remove(RedCharDeviceClientOpaque *client);
    int client_exists(RedCharDeviceClientOpaque *client);

    void start();
    void stop();
    SpiceServer* get_server();

    /** Read from device **/

    void wakeup();

    void send_to_client_tokens_add(RedCharDeviceClientOpaque *client,
                                   uint32_t tokens);


    void send_to_client_tokens_set(RedCharDeviceClientOpaque *client,
                                   uint32_t tokens);
    /** Write to device **/

    RedCharDeviceWriteBuffer *write_buffer_get_client(RedCharDeviceClientOpaque *client,
                                                      int size);

    /* Returns NULL if use_token == true and no tokens are available */
    RedCharDeviceWriteBuffer *write_buffer_get_server(int size, bool use_token);

    /* Either add the buffer to the write queue or release it */
    void write_buffer_add(RedCharDeviceWriteBuffer *write_buf);

    /* Release a buffer allocated.
     * This is static as potentially you can pass a null pointer for the object */
    static void write_buffer_release(RedCharDevice *dev,
                                     RedCharDeviceWriteBuffer **p_write_buf);

    SpiceCharDeviceInstance *get_device_instance();

    /**
     * Read data from device
     */
    int read(uint8_t *buf, int len);

    red::unique_link<RedCharDevicePrivate> priv;

//protected:
public:
    /*
     * Messages that are addressed to the client can be queued in case we have
     * multiple clients and some of them don't have enough tokens.
     */

    /* reads from the device till reaching a msg that should be sent to the client,
     * or till the reading fails */
    virtual RedPipeItemPtr read_one_msg_from_device() = 0;

    /* After this call, the message is unreferenced.
     * Can be NULL. */
    virtual void send_msg_to_client(RedPipeItem *msg, RedCharDeviceClientOpaque *client) {};

    /* The cb is called when a predefined number of write buffers were consumed by the
     * device */
    virtual void send_tokens_to_client(RedCharDeviceClientOpaque *client, uint32_t tokens);

    /* The cb is called when a server (self) message that was addressed to the device,
     * has been completely written to it */
    virtual void on_free_self_token() {};

    /* This cb is called if it is recommended to remove the client
     * due to slow flow or due to some other error.
     * The called instance should disconnect the client, or at least the corresponding channel */
    virtual void remove_client(RedCharDeviceClientOpaque *client) = 0;

    /* This cb is called when device receives an event */
    virtual void port_event(uint8_t event);

private:
    inline void write_buffer_release(RedCharDeviceWriteBuffer **p_write_buf)
    {
        write_buffer_release(this, p_write_buf);
    }
    int write_to_device();
    void init_device_instance();

    static void write_retry(RedCharDevice *dev);
};

/* api for specific char devices */

red::shared_ptr<RedCharDevice>
spicevmc_device_connect(RedsState *reds, SpiceCharDeviceInstance *sin, uint8_t channel_type);

SpiceCharDeviceInterface *spice_char_device_get_interface(SpiceCharDeviceInstance *instance);

#include "pop-visibility.h"

#endif /* CHAR_DEVICE_H_ */
