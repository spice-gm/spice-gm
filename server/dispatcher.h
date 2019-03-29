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

#ifndef DISPATCHER_H_
#define DISPATCHER_H_

#include <pthread.h>
#include <glib-object.h>

#include "red-common.h"

#define TYPE_DISPATCHER dispatcher_get_type()

#define DISPATCHER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_DISPATCHER, Dispatcher))
#define DISPATCHER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), TYPE_DISPATCHER, DispatcherClass))
#define IS_DISPATCHER(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), TYPE_DISPATCHER))
#define IS_DISPATCHER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), TYPE_DISPATCHER))
#define DISPATCHER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), TYPE_DISPATCHER, DispatcherClass))

typedef struct Dispatcher Dispatcher;
typedef struct DispatcherClass DispatcherClass;
typedef struct DispatcherPrivate DispatcherPrivate;

/* A Dispatcher provides inter-thread communication by serializing messages.
 * Currently the Dispatcher uses a unix socket (socketpair) for dispatching the
 * messages.
 *
 * Message types are identified by a unique integer value and must first be
 * registered with the class (see dispatcher_register_handler()) before they
 * can be sent. Sending threads can send a message using the
 * dispatcher_send_message() function. The receiving thread can monitor the
 * dispatcher's 'receive' file descriptor (see dispatcher_get_recv_fd()) for
 * activity and should call dispatcher_handle_recv_read() to process incoming
 * messages.
 */
struct Dispatcher
{
    GObject parent;

    DispatcherPrivate *priv;
};

struct DispatcherClass
{
    GObjectClass parent_class;
};

GType dispatcher_get_type(void) G_GNUC_CONST;

/* dispatcher_new
 *
 * Create a new Dispatcher object
 *
 * @max_message_type:   indicates the number of unique message types that can
 *                      be handled by this dispatcher. Each message type is
 *                      identified by an integer value between 0 and
 *                      max_message_type-1.
 */
Dispatcher *dispatcher_new(size_t max_message_type);


/* The function signature for handlers of a specific message type */
typedef void (*dispatcher_handle_message)(void *opaque,
                                          void *payload);

/* The signature for a function that handles all messages (see
 * dispatcher_register_universal_handler()) */
typedef void (*dispatcher_handle_any_message)(void *opaque,
                                              uint32_t message_type,
                                              void *payload);

/* dispatcher_send_message
 *
 * Sends a message to the receiving thread. The message type must have been
 * registered first (see dispatcher_register_handler()).  @payload must be a
 * buffer of the same size as the size registered for @message_type
 *
 * If the sent message is a message type requires an ACK, this function will
 * block until it receives an ACK from the receiving thread.
 *
 * @message_type: message type
 * @payload:      payload
 */
void dispatcher_send_message(Dispatcher *dispatcher, uint32_t message_type,
                             void *payload);

/* dispatcher_send_message_custom
 *
 * Sends a message to the receiving thread.
 *
 * If the sent message requires an ACK, this function will block until it
 * receives an ACK from the receiving thread.
 *
 * @handler:      callback to handle message
 * @payload:      payload
 * @payload_size: size of payload
 * @ack:          acknowledge required. Make message synchronous
 */
void dispatcher_send_message_custom(Dispatcher *dispatcher, dispatcher_handle_message handler,
                                    void *payload, uint32_t payload_size, bool ack);

/* dispatcher_register_handler
 *
 * This function registers a message type with the dispatcher, and registers
 * @handler as the function that will handle incoming messages of this type.
 * If @ack is true, the dispatcher will also send an ACK in response to the
 * message after the message has been passed to the handler. You can only
 * register a given message type once. For example, you cannot register two
 * different handlers for the same message type with different @ack values.
 *
 * @dispatcher:     dispatcher
 * @messsage_type:  message type
 * @handler:        message handler
 * @size:           message size. Each type has a fixed associated size.
 * @ack:            whether the dispatcher should send an ACK to the sender
 */
void dispatcher_register_handler(Dispatcher *dispatcher, uint32_t message_type,
                                 dispatcher_handle_message handler, size_t size,
                                 bool ack);

/* dispatcher_register_universal_handler
 *
 * Register a universal handler that will be called when *any* message is
 * received by the dispatcher. When a message is received, this handler will be
 * called first. If the received message type was registered via
 * dispatcher_register_handler(), the message-specific handler will then be
 * called. Only one universal handler can be registered. This feature can be
 * used to record all messages to a file for replay and debugging.
 *
 * @dispatcher:     dispatcher
 * @handler:        a handler function
 */
void dispatcher_register_universal_handler(Dispatcher *dispatcher,
                                    dispatcher_handle_any_message handler);

/* dispatcher_create_watch
 *
 * Create a new watch to handle events for the dispatcher.
 * You should release it before releasing the dispatcher.
 *
 * @return: newly created watch
 */
SpiceWatch *dispatcher_create_watch(Dispatcher *dispatcher, SpiceCoreInterfaceInternal *core);

/* dispatcher_set_opaque
 *
 * This @opaque pointer is user-defined data that will be passed as the first
 * argument to all handler functions.
 *
 * @dispatcher: Dispatcher instance
 * @opaque: opaque to use for callbacks
 */
void dispatcher_set_opaque(Dispatcher *dispatcher, void *opaque);

/* dispatcher_get_thread_id
 *
 * Returns the id of the thread that created this Dispatcher object
 */
pthread_t dispatcher_get_thread_id(Dispatcher *self);

#endif /* DISPATCHER_H_ */
