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

/* This header should contains internal details between RedQxl and
 * RedWorker.
 * Should be included only by red-worker.cpp, red-qxl.cpp and
 * red-replay-qxl.cpp (which uses message values).
 */

#ifndef RED_WORKER_H_
#define RED_WORKER_H_

#include "red-channel.h"

#include "push-visibility.h"

struct RedWorker;

RedWorker* red_worker_new(QXLInstance *qxl);
bool       red_worker_run(RedWorker *worker);
void red_worker_free(RedWorker *worker);

struct Dispatcher *red_qxl_get_dispatcher(QXLInstance *qxl);
void red_qxl_destroy_primary_surface_complete(QXLState *qxl_state);
void red_qxl_create_primary_surface_complete(QXLState *qxl_state, const QXLDevSurfaceCreate* surface);
bool red_qxl_is_running(QXLInstance *qxl);
void red_qxl_set_running(QXLInstance *qxl, bool running);

using RedWorkerMessage = uint32_t;

/* Keep message order, only append new messages!
 * Replay code store enum values into save files.
 */
enum {
    RED_WORKER_MESSAGE_NOP,

    RED_WORKER_MESSAGE_UPDATE,
    RED_WORKER_MESSAGE_WAKEUP,
    RED_WORKER_MESSAGE_OOM,
    RED_WORKER_MESSAGE_READY, /* unused */

    RED_WORKER_MESSAGE_DISPLAY_CONNECT_DEPRECATED,
    RED_WORKER_MESSAGE_DISPLAY_DISCONNECT_DEPRECATED,
    RED_WORKER_MESSAGE_DISPLAY_MIGRATE_DEPRECATED,
    RED_WORKER_MESSAGE_START,
    RED_WORKER_MESSAGE_STOP,
    RED_WORKER_MESSAGE_CURSOR_CONNECT_DEPRECATED,
    RED_WORKER_MESSAGE_CURSOR_DISCONNECT_DEPRECATED,
    RED_WORKER_MESSAGE_CURSOR_MIGRATE_DEPRECATED,
    RED_WORKER_MESSAGE_SET_COMPRESSION,
    RED_WORKER_MESSAGE_SET_STREAMING_VIDEO,
    RED_WORKER_MESSAGE_SET_MOUSE_MODE,
    RED_WORKER_MESSAGE_ADD_MEMSLOT,
    RED_WORKER_MESSAGE_DEL_MEMSLOT,
    RED_WORKER_MESSAGE_RESET_MEMSLOTS,
    RED_WORKER_MESSAGE_DESTROY_SURFACES,
    RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE,
    RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE,
    RED_WORKER_MESSAGE_RESET_CURSOR,
    RED_WORKER_MESSAGE_RESET_IMAGE_CACHE,
    RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT,
    RED_WORKER_MESSAGE_LOADVM_COMMANDS,
    /* async commands */
    RED_WORKER_MESSAGE_UPDATE_ASYNC,
    RED_WORKER_MESSAGE_ADD_MEMSLOT_ASYNC,
    RED_WORKER_MESSAGE_DESTROY_SURFACES_ASYNC,
    RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE_ASYNC,
    RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE_ASYNC,
    RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT_ASYNC,
    /* suspend/windows resolution change command */
    RED_WORKER_MESSAGE_FLUSH_SURFACES_ASYNC,

    RED_WORKER_MESSAGE_DISPLAY_CHANNEL_CREATE, /* unused */
    RED_WORKER_MESSAGE_CURSOR_CHANNEL_CREATE, /* unused */

    RED_WORKER_MESSAGE_MONITORS_CONFIG_ASYNC,
    RED_WORKER_MESSAGE_DRIVER_UNLOAD,
    RED_WORKER_MESSAGE_GL_SCANOUT,
    RED_WORKER_MESSAGE_GL_DRAW_ASYNC,
    RED_WORKER_MESSAGE_SET_VIDEO_CODECS,

    /* close worker thread */
    RED_WORKER_MESSAGE_CLOSE_WORKER,

    RED_WORKER_MESSAGE_COUNT // LAST
};

struct RedWorkerMessageUpdate {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_UPDATE };
    uint32_t surface_id;
    QXLRect * qxl_area;
    QXLRect * qxl_dirty_rects;
    uint32_t num_dirty_rects;
    uint32_t clear_dirty_region;
};

struct RedWorkerMessageAsync {
    uint64_t cookie;
};

struct RedWorkerMessageUpdateAsync {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_UPDATE_ASYNC };
    RedWorkerMessageAsync base;
    uint32_t surface_id;
    QXLRect qxl_area;
    uint32_t clear_dirty_region;
};

struct RedWorkerMessageAddMemslot {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_ADD_MEMSLOT };
    QXLDevMemSlot mem_slot;
};

struct RedWorkerMessageAddMemslotAsync {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_ADD_MEMSLOT_ASYNC };
    RedWorkerMessageAsync base;
    QXLDevMemSlot mem_slot;
};

struct RedWorkerMessageDelMemslot {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_DEL_MEMSLOT };
    uint32_t slot_group_id;
    uint32_t slot_id;
};

struct RedWorkerMessageDestroySurfaces {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_DESTROY_SURFACES };
    uint8_t dummy_empty_field[0]; // C/C++ compatibility
};

struct RedWorkerMessageDestroySurfacesAsync {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_DESTROY_SURFACES_ASYNC };
    RedWorkerMessageAsync base;
};


struct RedWorkerMessageDestroyPrimarySurface {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE };
    uint32_t surface_id;
};

struct RedWorkerMessageDestroyPrimarySurfaceAsync {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE_ASYNC };
    RedWorkerMessageAsync base;
    uint32_t surface_id;
};

struct RedWorkerMessageCreatePrimarySurfaceAsync {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE_ASYNC };
    RedWorkerMessageAsync base;
    uint32_t surface_id;
    QXLDevSurfaceCreate surface;
};

struct RedWorkerMessageCreatePrimarySurface {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE };
    uint32_t surface_id;
    QXLDevSurfaceCreate surface;
};

struct RedWorkerMessageResetImageCache {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_RESET_IMAGE_CACHE };
    uint8_t dummy_empty_field[0]; // C/C++ compatibility
};

struct RedWorkerMessageResetCursor {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_RESET_CURSOR };
    uint8_t dummy_empty_field[0]; // C/C++ compatibility
};

struct RedWorkerMessageWakeup {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_WAKEUP };
    uint8_t dummy_empty_field[0]; // C/C++ compatibility
};

struct RedWorkerMessageOom {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_OOM };
    uint8_t dummy_empty_field[0]; // C/C++ compatibility
};

struct RedWorkerMessageStart {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_START };
    uint8_t dummy_empty_field[0]; // C/C++ compatibility
};

struct RedWorkerMessageFlushSurfacesAsync {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_FLUSH_SURFACES_ASYNC };
    RedWorkerMessageAsync base;
};

struct RedWorkerMessageStop {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_STOP };
    uint8_t dummy_empty_field[0]; // C/C++ compatibility
};

/* this command is sync, so it's ok to pass a pointer */
struct RedWorkerMessageLoadvmCommands {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_LOADVM_COMMANDS };
    uint32_t count;
    QXLCommandExt *ext;
};

struct RedWorkerMessageSetCompression {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_SET_COMPRESSION };
    SpiceImageCompression image_compression;
};

struct RedWorkerMessageSetStreamingVideo {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_SET_STREAMING_VIDEO };
    uint32_t streaming_video;
};

struct RedWorkerMessageSetVideoCodecs {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_SET_VIDEO_CODECS };
    GArray* video_codecs;
};

struct RedWorkerMessageSetMouseMode {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_SET_MOUSE_MODE };
    uint32_t mode;
};

struct RedWorkerMessageDestroySurfaceWait {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT };
    uint32_t surface_id;
};

struct RedWorkerMessageDestroySurfaceWaitAsync {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT_ASYNC };
    RedWorkerMessageAsync base;
    uint32_t surface_id;
};

struct RedWorkerMessageResetMemslots {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_RESET_MEMSLOTS };
};

struct RedWorkerMessageMonitorsConfigAsync {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_MONITORS_CONFIG_ASYNC };
    RedWorkerMessageAsync base;
    QXLPHYSICAL monitors_config;
    int group_id;
    unsigned int max_monitors;
};

struct RedWorkerMessageDriverUnload {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_DRIVER_UNLOAD };
    uint8_t dummy_empty_field[0]; // C/C++ compatibility
};

struct RedWorkerMessageGlScanout {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_GL_SCANOUT };
    uint8_t dummy_empty_field[0]; // C/C++ compatibility
};

struct RedWorkerMessageClose {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_CLOSE_WORKER };
    uint8_t dummy_empty_field[0]; // C/C++ compatibility
};

struct RedWorkerMessageGlDraw {
    enum { MESSAGE_NUM = RED_WORKER_MESSAGE_GL_DRAW_ASYNC };
    SpiceMsgDisplayGlDraw draw;
};

enum {
    RED_DISPATCHER_PENDING_WAKEUP,
    RED_DISPATCHER_PENDING_OOM,
};

void red_qxl_clear_pending(QXLState *qxl_state, int pending);

#include "pop-visibility.h"

#endif /* RED_WORKER_H_ */
