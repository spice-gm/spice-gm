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

#ifndef MAIN_DISPATCHER_H_
#define MAIN_DISPATCHER_H_

#include "spice.h"
#include "dispatcher.h"
#include "red-channel.h"

#define TYPE_MAIN_DISPATCHER main_dispatcher_get_type()

#define MAIN_DISPATCHER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_MAIN_DISPATCHER, MainDispatcher))
#define MAIN_DISPATCHER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), TYPE_MAIN_DISPATCHER, MainDispatcherClass))
#define IS_MAIN_DISPATCHER(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), TYPE_MAIN_DISPATCHER))
#define IS_MAIN_DISPATCHER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), TYPE_MAIN_DISPATCHER))
#define MAIN_DISPATCHER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), TYPE_MAIN_DISPATCHER, MainDispatcherClass))

typedef struct MainDispatcher MainDispatcher;
typedef struct MainDispatcherClass MainDispatcherClass;
typedef struct MainDispatcherPrivate MainDispatcherPrivate;

struct MainDispatcher
{
    Dispatcher parent;

    MainDispatcherPrivate *priv;
};

struct MainDispatcherClass
{
    DispatcherClass parent_class;
};

GType main_dispatcher_get_type(void) G_GNUC_CONST;

void main_dispatcher_channel_event(MainDispatcher *self, int event, SpiceChannelEventInfo *info);
void main_dispatcher_seamless_migrate_dst_complete(MainDispatcher *self, RedClient *client);
void main_dispatcher_set_mm_time_latency(MainDispatcher *self, RedClient *client, uint32_t latency);
/*
 * Disconnecting the client is always executed asynchronously,
 * in order to protect from expired references in the routines
 * that triggered the client destruction.
 */
void main_dispatcher_client_disconnect(MainDispatcher *self, RedClient *client);

MainDispatcher* main_dispatcher_new(RedsState *reds);

#endif /* MAIN_DISPATCHER_H_ */
