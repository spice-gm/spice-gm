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
*/

#ifndef RED_COMMON_H_
#define RED_COMMON_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <spice/macros.h>
#include <common/log.h>
#include <common/lz_common.h>
#include <common/marshaller.h>
#include <common/messages.h>
#include <common/draw.h>
#include <common/verify.h>

#include "spice-wrapped.h"
#include "utils.h"
#include "sys-socket.h"

#define SPICE_UPCAST(type, ptr) \
    (verify_expr(SPICE_OFFSETOF(type, base) == 0,SPICE_CONTAINEROF(ptr, type, base)))

SPICE_BEGIN_DECLS

void red_timer_start(SpiceTimer *timer, uint32_t ms);
void red_timer_cancel(SpiceTimer *timer);
void red_timer_remove(SpiceTimer *timer);
void red_watch_update_mask(SpiceWatch *watch, int event_mask);
void red_watch_remove(SpiceWatch *watch);

typedef struct SpiceCoreInterfaceInternal SpiceCoreInterfaceInternal;

extern const SpiceCoreInterfaceInternal event_loop_core;
extern const SpiceCoreInterfaceInternal core_interface_adapter;

SPICE_END_DECLS

struct SpiceCoreInterfaceInternal {
    SpiceTimer *(*timer_add)(const SpiceCoreInterfaceInternal *iface, SpiceTimerFunc func, void *opaque);

    SpiceWatch *(*watch_add)(const SpiceCoreInterfaceInternal *iface, int fd, int event_mask, SpiceWatchFunc func, void *opaque);

    void (*channel_event)(const SpiceCoreInterfaceInternal *iface, int event, SpiceChannelEventInfo *info);

#ifdef __cplusplus
    template <typename T>
    inline SpiceTimer *timer_new(void (*func)(T*), T *opaque) const
    { return this->timer_add(this, (SpiceTimerFunc) func, opaque); }

    template <typename T>
    inline SpiceWatch *watch_new(int fd, int event_mask, void (*func)(int,int,T*), T* opaque) const
    { return this->watch_add(this, fd, event_mask, (SpiceWatchFunc) func, opaque); }
#endif


    /* This structure is an adapter that allows us to use the same API to
     * implement the core interface in a couple different ways. The first
     * method is to use a public SpiceCoreInterface provided to us by the
     * library user (for example, qemu). The second method is to implement the
     * core interface functions using the glib event loop. In order to avoid
     * global variables, each method needs to store additional data in this
     * adapter structure. Instead of using a generic void* data parameter, we
     * provide a bit more type-safety by using a union to store the type of
     * data needed by each implementation.
     */
    union {
        GMainContext *main_context;
        SpiceCoreInterface *public_interface;
    };
};

typedef struct RedsState RedsState;

typedef struct GListIter {
    GList *link;
    GList *next;
} GListIter;

/* Iterate through a GList. Note that the iteration is "safe" meaning that the
 * current item can be removed while the list is scanned. This is required as
 * the code inside the loop in some cases can remove the element we are
 * processing */
#define GLIST_FOREACH_GENERIC(_list, _iter, _type, _data, _dir) \
    for (GListIter _iter = { .link = _list }; \
        (_data = (_type *) (_iter.link ? _iter.link->data : NULL), \
         _iter.next = (_iter.link ? _iter.link->_dir : NULL), \
         _iter.link) != NULL; \
         _iter.link = _iter.next)

#define GLIST_FOREACH(_list, _type, _data) \
    GLIST_FOREACH_GENERIC(_list, G_PASTE(_iter_, __LINE__), _type, _data, next)

#define GLIST_FOREACH_REVERSED(_list, _type, _data) \
    GLIST_FOREACH_GENERIC(_list, G_PASTE(_iter_, __LINE__), _type, _data, prev)

/* This macro allows to use GLib for a class hierarchy allocation.
 * The aims are:
 * - do not depend on C++ runtime, just C;
 * - do not throw memory exception from a C library;
 * - zero out the structure like GOject did, we are not still
 *   initializing automatically all members;
 * - do not allow to allocate array of this type, do not mix fine
 *   with reference counting and inheritance.
 */
#define SPICE_CXX_GLIB_ALLOCATOR \
    void *operator new(size_t size) { return g_malloc0(size); } \
    void operator delete(void *p) { g_free(p); } \
    void* operator new[](size_t count);

// XXX todo remove, just for easy portability
#define XXX_CAST(from, to, name) \
static inline to* name(from *p) { \
    return p ? static_cast<to*>(p) : nullptr; \
}

#endif /* RED_COMMON_H_ */
