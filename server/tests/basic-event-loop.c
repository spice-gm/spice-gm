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
#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <signal.h>
#include <string.h>

#include "red-common.h"
#include "spice/macros.h"
#include "basic-event-loop.h"

int debug = 0;

#define DPRINTF(x, format, ...) { \
    if (x <= debug) { \
        printf("%s: " format "\n" , __FUNCTION__, ## __VA_ARGS__); \
    } \
}

static SpiceCoreInterfaceInternal base_core_interface;
static GMainContext *main_context = NULL;
static GMainLoop *loop = NULL;

GMainContext *basic_event_loop_get_context(void)
{
    return main_context;
}

static void event_loop_channel_event(int event, SpiceChannelEventInfo *info)
{
    DPRINTF(1, "channel event con, type, id, event: %d, %d, %d, %d",
            info->connection_id, info->type, info->id, event);
}

void basic_event_loop_mainloop(void)
{
    loop = g_main_loop_new(main_context, FALSE);

    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    loop = NULL;
}

void basic_event_loop_quit(void)
{
    if (loop) {
        g_main_loop_quit(loop);
    }
}

static void ignore_sigpipe(void)
{
#ifndef _WIN32
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    sigfillset(&act.sa_mask);
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);
#endif
}

static SpiceTimer* base_timer_add(SpiceTimerFunc func, void *opaque)
{
    return base_core_interface.timer_add(&base_core_interface, func, opaque);
}

static SpiceWatch *base_watch_add(int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    return base_core_interface.watch_add(&base_core_interface, fd, event_mask, func, opaque);
}

static SpiceCoreInterface core = {
    .base = {
        .type = NULL,
        .description = NULL,
        .major_version = SPICE_INTERFACE_CORE_MAJOR,
        .minor_version = SPICE_INTERFACE_CORE_MINOR,
    },
    .timer_add = base_timer_add,
    .timer_start = red_timer_start,
    .timer_cancel = red_timer_cancel,
    .timer_remove = red_timer_remove,
    .watch_add = base_watch_add,
    .watch_update_mask = red_watch_update_mask,
    .watch_remove = red_watch_remove,
    .channel_event = event_loop_channel_event,
};

SpiceCoreInterface *basic_event_loop_init(void)
{
    ignore_sigpipe();
    spice_assert(main_context == NULL);
    /* Qemu can use a context which is not the default one so to make
     * sure we can handle this condition here we emulate it so don't
     * use g_main_context_default */
    main_context = g_main_context_new();
    base_core_interface = event_loop_core;
    base_core_interface.main_context = main_context;

    return &core;
}

void basic_event_loop_destroy(void)
{
    spice_assert(main_context != NULL);
    g_main_context_unref(main_context);
    main_context = NULL;
}
