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

/*
 * This file exports a global variable:
 *
 * const SpiceCoreInterfaceInternal event_loop_core;
 */
#include <config.h>

#include "red-common.h"

typedef struct SpiceCoreFuncs {
    void (*timer_start)(SpiceTimer *timer, uint32_t ms);
    void (*timer_cancel)(SpiceTimer *timer);
    void (*timer_remove)(SpiceTimer *timer);
    void (*watch_update_mask)(SpiceWatch *watch, int event_mask);
    void (*watch_remove)(SpiceWatch *watch);
} SpiceCoreFuncs;

struct SpiceTimer {
    const SpiceCoreFuncs *funcs;
};

struct SpiceWatch {
    const SpiceCoreFuncs *funcs;
};

void red_timer_start(SpiceTimer *timer, uint32_t ms)
{
    if (timer) {
        timer->funcs->timer_start(timer, ms);
    }
}

void red_timer_cancel(SpiceTimer *timer)
{
    if (timer) {
        timer->funcs->timer_cancel(timer);
    }
}

void red_timer_remove(SpiceTimer *timer)
{
    if (timer) {
        timer->funcs->timer_remove(timer);
    }
}

void red_watch_update_mask(SpiceWatch *watch, int event_mask)
{
    if (watch) {
        watch->funcs->watch_update_mask(watch, event_mask);
    }
}

void red_watch_remove(SpiceWatch *watch)
{
    if (watch) {
        watch->funcs->watch_remove(watch);
    }
}

static const SpiceCoreFuncs glib_core_funcs;

typedef struct SpiceTimerGlib {
    SpiceTimer base;
    GMainContext *context;
    SpiceTimerFunc func;
    void *opaque;
    GSource *source;
} SpiceTimerGlib;

static SpiceTimer* timer_add(const SpiceCoreInterfaceInternal *iface,
                             SpiceTimerFunc func, void *opaque)
{
    SpiceTimerGlib *timer = g_new0(SpiceTimerGlib, 1);

    timer->base.funcs = &glib_core_funcs;
    timer->context = iface->main_context;
    timer->func = func;
    timer->opaque = opaque;

    return &timer->base;
}

static gboolean timer_func(gpointer user_data)
{
    SpiceTimerGlib *timer = (SpiceTimerGlib*) user_data;

    timer->func(timer->opaque);
    /* timer might be free after func(), don't touch */

    return FALSE;
}

static void timer_cancel(SpiceTimer *timer_base)
{
    SpiceTimerGlib *timer = SPICE_UPCAST(SpiceTimerGlib, timer_base);
    if (timer->source) {
        g_source_destroy(timer->source);
        g_source_unref(timer->source);
        timer->source = NULL;
    }
}

static void timer_start(SpiceTimer *timer_base, uint32_t ms)
{
    timer_cancel(timer_base);

    SpiceTimerGlib *timer = SPICE_UPCAST(SpiceTimerGlib, timer_base);

    timer->source = g_timeout_source_new(ms);
    spice_assert(timer->source != NULL);

    g_source_set_callback(timer->source, timer_func, timer, NULL);

    g_source_attach(timer->source, timer->context);
}

static void timer_remove(SpiceTimer *timer_base)
{
    timer_cancel(timer_base);

    SpiceTimerGlib *timer = SPICE_UPCAST(SpiceTimerGlib, timer_base);
    spice_assert(timer->source == NULL);
    g_free(timer);
}

static GIOCondition spice_event_to_giocondition(int event_mask)
{
    GIOCondition condition = 0;

    if (event_mask & SPICE_WATCH_EVENT_READ)
        condition |= G_IO_IN;
    if (event_mask & SPICE_WATCH_EVENT_WRITE)
        condition |= G_IO_OUT;

    return condition;
}

static int giocondition_to_spice_event(GIOCondition condition)
{
    int event = 0;

    if (condition & G_IO_IN)
        event |= SPICE_WATCH_EVENT_READ;
    if (condition & G_IO_OUT)
        event |= SPICE_WATCH_EVENT_WRITE;

    return event;
}

#ifdef _WIN32
typedef struct SpiceWatchGlib {
    SpiceWatch base;
    GMainContext *context;
    void *opaque;
    GSource *source;
    GIOChannel *channel;
    SpiceWatchFunc func;
} SpiceWatchGlib;

static gboolean watch_func(GIOChannel *source, GIOCondition condition,
                           gpointer data)
{
    SpiceWatchGlib *watch = (SpiceWatchGlib*) data;
    // this works also under Windows despite the name
    int fd = g_io_channel_unix_get_fd(source);

    watch->func(fd, giocondition_to_spice_event(condition), watch->opaque);

    return TRUE;
}

static void watch_update_mask(SpiceWatch *watch_base, int event_mask)
{
    SpiceWatchGlib *watch = SPICE_UPCAST(SpiceWatchGlib, watch_base);
    if (watch->source) {
        g_source_destroy(watch->source);
        g_source_unref(watch->source);
        watch->source = NULL;
    }

    if (!event_mask)
        return;

    watch->source = g_io_create_watch(watch->channel, spice_event_to_giocondition(event_mask));
    /* g_source_set_callback() documentation says:
     * "The exact type of func depends on the type of source; ie. you should
     *  not count on func being called with data as its first parameter."
     * In this case it is a GIOFunc. First cast to GIOFunc to make sure it is the right type.
     * The other casts silence the warning from gcc */
    g_source_set_callback(watch->source, (GSourceFunc)(void*)(GIOFunc)watch_func, watch, NULL);
    g_source_attach(watch->source, watch->context);
}

static SpiceWatch *watch_add(const SpiceCoreInterfaceInternal *iface,
                             int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    SpiceWatchGlib *watch;

    spice_return_val_if_fail(fd != -1, NULL);
    spice_return_val_if_fail(func != NULL, NULL);

    watch = g_new0(SpiceWatchGlib, 1);
    watch->base.funcs = &glib_core_funcs;
    watch->context = iface->main_context;
    watch->channel = g_io_channel_win32_new_socket(fd);
    watch->func = func;
    watch->opaque = opaque;

    watch_update_mask(&watch->base, event_mask);

    return &watch->base;
}

static void watch_remove(SpiceWatch *watch_base)
{
    SpiceWatchGlib *watch = SPICE_UPCAST(SpiceWatchGlib, watch_base);
    watch_update_mask(watch_base, 0);
    spice_assert(watch->source == NULL);

    g_io_channel_unref(watch->channel);
    g_free(watch);
}

#else

typedef struct SpiceWatchGlib {
    GSource source;
    SpiceWatch spice_base;
    gpointer unix_fd;
    int fd;
} SpiceWatchGlib;

static gboolean
spice_watch_check(GSource *source)
{
    SpiceWatchGlib *watch = SPICE_CONTAINEROF(source, SpiceWatchGlib, source);

    return g_source_query_unix_fd(&watch->source, watch->unix_fd) != 0;
}

static gboolean
spice_watch_dispatch(GSource     *source,
                     GSourceFunc  callback,
                     gpointer     user_data)
{
    SpiceWatchGlib *watch = SPICE_CONTAINEROF(source, SpiceWatchGlib, source);
    SpiceWatchFunc func = (SpiceWatchFunc)(void*) callback;
    GIOCondition condition = g_source_query_unix_fd(&watch->source, watch->unix_fd);

    func(watch->fd, giocondition_to_spice_event(condition), user_data);
    /* timer might be free after func(), don't touch */

    return G_SOURCE_CONTINUE;
}

static GSourceFuncs spice_watch_funcs = {
    .check = spice_watch_check,
    .dispatch = spice_watch_dispatch,
};

static void watch_update_mask(SpiceWatch *watch_base, int event_mask)
{
    SpiceWatchGlib *watch = SPICE_CONTAINEROF(watch_base, SpiceWatchGlib, spice_base);
    GIOCondition condition = spice_event_to_giocondition(event_mask);

    g_source_modify_unix_fd(&watch->source, watch->unix_fd, condition);
}

static SpiceWatch *watch_add(const SpiceCoreInterfaceInternal *iface,
                             int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    SPICE_VERIFY(SPICE_OFFSETOF(SpiceWatchGlib, source) == 0);
    SpiceWatchGlib *watch =
        (SpiceWatchGlib *) g_source_new(&spice_watch_funcs, sizeof(SpiceWatchGlib));

    spice_return_val_if_fail(fd != -1, NULL);
    spice_return_val_if_fail(func != NULL, NULL);

    watch->spice_base.funcs = &glib_core_funcs;
    watch->fd = fd;

    g_source_set_callback(&watch->source, (GSourceFunc)(void*)(SpiceWatchFunc) func, opaque, NULL);

    g_source_attach(&watch->source, iface->main_context);

    GIOCondition condition = spice_event_to_giocondition(event_mask);
    watch->unix_fd = g_source_add_unix_fd(&watch->source, watch->fd, condition);

    return &watch->spice_base;
}

static void watch_remove(SpiceWatch *watch_base)
{
    SpiceWatchGlib *watch = SPICE_CONTAINEROF(watch_base, SpiceWatchGlib, spice_base);

    g_source_remove_unix_fd(&watch->source, watch->unix_fd);
    g_source_destroy(&watch->source);
    g_source_unref(&watch->source);
}
#endif

static const SpiceCoreFuncs glib_core_funcs = {
    .timer_start = timer_start,
    .timer_cancel = timer_cancel,
    .timer_remove = timer_remove,

    .watch_update_mask = watch_update_mask,
    .watch_remove = watch_remove,
};

const SpiceCoreInterfaceInternal event_loop_core = {
    .timer_add = timer_add,
    .watch_add = watch_add,
};

/*
 * Adapter for SpiceCodeInterface
 */

static const SpiceCoreFuncs qemu_core_funcs;

typedef struct SpiceTimerQemu {
    SpiceTimer base;
    SpiceCoreInterface *core;
    SpiceTimer *qemu_timer;
} SpiceTimerQemu;

static SpiceTimer *adapter_timer_add(const SpiceCoreInterfaceInternal *iface, SpiceTimerFunc func, void *opaque)
{
    SpiceTimerQemu *timer = g_new0(SpiceTimerQemu, 1);

    timer->base.funcs = &qemu_core_funcs;
    timer->core = iface->public_interface;
    timer->qemu_timer = timer->core->timer_add(func, opaque);
    return &timer->base;
}

static void adapter_timer_start(SpiceTimer *timer_, uint32_t ms)
{
    SpiceTimerQemu *timer = SPICE_UPCAST(SpiceTimerQemu, timer_);
    timer->core->timer_start(timer->qemu_timer, ms);
}

static void adapter_timer_cancel(SpiceTimer *timer_)
{
    SpiceTimerQemu *timer = SPICE_UPCAST(SpiceTimerQemu, timer_);
    timer->core->timer_cancel(timer->qemu_timer);
}

static void adapter_timer_remove(SpiceTimer *timer_)
{
    SpiceTimerQemu *timer = SPICE_UPCAST(SpiceTimerQemu, timer_);
    timer->core->timer_remove(timer->qemu_timer);
    g_free(timer);
}

typedef struct SpiceWatchQemu {
    SpiceWatch base;
    SpiceCoreInterface *core;
    SpiceWatch *qemu_watch;
} SpiceWatchQemu;

static SpiceWatch *adapter_watch_add(const SpiceCoreInterfaceInternal *iface,
                                     int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    // note: Qemu API is fine having a SOCKET on Windows
    SpiceWatchQemu *watch = g_new0(SpiceWatchQemu, 1);

    watch->base.funcs = &qemu_core_funcs;
    watch->core = iface->public_interface;
    watch->qemu_watch = watch->core->watch_add(fd, event_mask, func, opaque);
    return &watch->base;
}

static void adapter_watch_update_mask(SpiceWatch *watch_, int event_mask)
{
    SpiceWatchQemu *watch = SPICE_UPCAST(SpiceWatchQemu, watch_);
    watch->core->watch_update_mask(watch->qemu_watch, event_mask);
}

static void adapter_watch_remove(SpiceWatch *watch_)
{
    SpiceWatchQemu *watch = SPICE_UPCAST(SpiceWatchQemu, watch_);
    watch->core->watch_remove(watch->qemu_watch);
    g_free(watch);
}

static void adapter_channel_event(const SpiceCoreInterfaceInternal *iface, int event, SpiceChannelEventInfo *info)
{
    if (iface->public_interface->base.minor_version >= 3 && iface->public_interface->channel_event != NULL)
        iface->public_interface->channel_event(event, info);
}

static const SpiceCoreFuncs qemu_core_funcs = {
    .timer_start = adapter_timer_start,
    .timer_cancel = adapter_timer_cancel,
    .timer_remove = adapter_timer_remove,

    .watch_update_mask = adapter_watch_update_mask,
    .watch_remove = adapter_watch_remove,
};

const SpiceCoreInterfaceInternal core_interface_adapter = {
    .timer_add = adapter_timer_add,
    .watch_add = adapter_watch_add,
    .channel_event = adapter_channel_event,
};
