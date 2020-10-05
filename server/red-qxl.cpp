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
#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>

#include <spice/qxl_dev.h>
#include <common/quic.h>
#include <common/sw_canvas.h>

#include "spice-wrapped.h"
#include "red-worker.h"
#include "reds.h"
#include "dispatcher.h"
#include "red-parse-qxl.h"
#include "red-channel-client.h"
#include "display-limits.h"

#include "red-qxl.h"


#define MAX_MONITORS_COUNT 16

struct QXLState {
    SPICE_CXX_GLIB_ALLOCATOR
    QXLInstance *qxl;
    red::shared_ptr<Dispatcher> dispatcher;
    uint32_t pending;
    int primary_active;
    int x_res;
    int y_res;
    int use_hardware_cursor;
    unsigned int max_monitors;
    RedsState *reds;
    RedWorker *worker;
    char device_address[MAX_DEVICE_ADDRESS_LEN];
    uint32_t device_display_ids[MAX_MONITORS_COUNT];
    size_t monitors_count;  // length of ^^^

    bool running;

    pthread_mutex_t scanout_mutex;
    SpiceMsgDisplayGlScanoutUnix scanout;
    uint64_t gl_draw_cookie;
};

#define GL_DRAW_COOKIE_INVALID (~((uint64_t) 0))

/* used by RedWorker */
bool red_qxl_is_running(QXLInstance *qxl)
{
    return qxl->st->running;
}

/* used by RedWorker */
void red_qxl_set_running(QXLInstance *qxl, bool running)
{
    qxl->st->running = running;
}

int red_qxl_check_qxl_version(QXLInstance *qxl, int major, int minor)
{
    int qxl_major = qxl_get_interface(qxl)->base.major_version;
    int qxl_minor = qxl_get_interface(qxl)->base.minor_version;

    return ((qxl_major > major) ||
            ((qxl_major == major) && (qxl_minor >= minor)));
}

SPICE_GNUC_VISIBLE
void spice_qxl_update_area(QXLInstance *instance, uint32_t surface_id,
                           struct QXLRect *qxl_area, struct QXLRect *qxl_dirty_rects,
                           uint32_t num_dirty_rects, uint32_t clear_dirty_region)
{
    RedWorkerMessageUpdate payload = {0,};

    payload.surface_id = surface_id;
    payload.qxl_area = qxl_area;
    payload.qxl_dirty_rects = qxl_dirty_rects;
    payload.num_dirty_rects = num_dirty_rects;
    payload.clear_dirty_region = clear_dirty_region;
    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_UPDATE, &payload);
}

gboolean red_qxl_client_monitors_config(QXLInstance *qxl,
                                        VDAgentMonitorsConfig *monitors_config)
{
    return (red_qxl_check_qxl_version(qxl, 3, 3) &&
        qxl_get_interface(qxl)->client_monitors_config &&
        qxl_get_interface(qxl)->client_monitors_config(qxl, monitors_config));
}

SPICE_GNUC_VISIBLE
void spice_qxl_update_area_async(QXLInstance *instance, uint32_t surface_id, QXLRect *qxl_area,
                                 uint32_t clear_dirty_region, uint64_t cookie)
{
    RedWorkerMessage message = RED_WORKER_MESSAGE_UPDATE_ASYNC;
    RedWorkerMessageUpdateAsync payload;

    payload.base.cookie = cookie;
    payload.surface_id = surface_id;
    payload.qxl_area = *qxl_area;
    payload.clear_dirty_region = clear_dirty_region;
    instance->st->dispatcher->send_message(message, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_add_memslot(QXLInstance *instance, QXLDevMemSlot *mem_slot)
{
    RedWorkerMessageAddMemslot payload;

    payload.mem_slot = *mem_slot;
    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_ADD_MEMSLOT, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_add_memslot_async(QXLInstance *instance, QXLDevMemSlot *mem_slot, uint64_t cookie)
{
    RedWorkerMessageAddMemslotAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_ADD_MEMSLOT_ASYNC;

    payload.base.cookie = cookie;
    payload.mem_slot = *mem_slot;
    instance->st->dispatcher->send_message(message, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_del_memslot(QXLInstance *instance, uint32_t slot_group_id, uint32_t slot_id)
{
    RedWorkerMessageDelMemslot payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DEL_MEMSLOT;

    payload.slot_group_id = slot_group_id;
    payload.slot_id = slot_id;
    instance->st->dispatcher->send_message(message, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_surfaces(QXLInstance *instance)
{
    RedWorkerMessageDestroySurfaces payload;

    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_DESTROY_SURFACES, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_surfaces_async(QXLInstance *instance, uint64_t cookie)
{
    RedWorkerMessageDestroySurfacesAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DESTROY_SURFACES_ASYNC;

    payload.base.cookie = cookie;
    instance->st->dispatcher->send_message(message, &payload);
}

/* used by RedWorker */
void red_qxl_destroy_primary_surface_complete(QXLState *qxl_state)
{
    qxl_state->x_res = 0;
    qxl_state->y_res = 0;
    qxl_state->use_hardware_cursor = FALSE;
    qxl_state->primary_active = FALSE;

    reds_update_client_mouse_allowed(qxl_state->reds);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_primary_surface(QXLInstance *instance, uint32_t surface_id)
{
    RedWorkerMessageDestroyPrimarySurface payload;
    payload.surface_id = surface_id;
    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE, &payload);
    red_qxl_destroy_primary_surface_complete(instance->st);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_primary_surface_async(QXLInstance *instance,
                                             uint32_t surface_id, uint64_t cookie)
{
    RedWorkerMessageDestroyPrimarySurfaceAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE_ASYNC;

    payload.base.cookie = cookie;
    payload.surface_id = surface_id;
    instance->st->dispatcher->send_message(message, &payload);
}

/* used by RedWorker */
void red_qxl_create_primary_surface_complete(QXLState *qxl_state, const QXLDevSurfaceCreate *surface)
{
    qxl_state->x_res = surface->width;
    qxl_state->y_res = surface->height;
    // mouse_mode is a boolean value, enforce it
    qxl_state->use_hardware_cursor = !!surface->mouse_mode;
    qxl_state->primary_active = TRUE;

    reds_update_client_mouse_allowed(qxl_state->reds);
}

SPICE_GNUC_VISIBLE
void spice_qxl_create_primary_surface_async(QXLInstance *instance, uint32_t surface_id,
                                            QXLDevSurfaceCreate *surface, uint64_t cookie)
{
    RedWorkerMessageCreatePrimarySurfaceAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE_ASYNC;

    payload.base.cookie = cookie;
    payload.surface_id = surface_id;
    payload.surface = *surface;
    instance->st->dispatcher->send_message(message, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_create_primary_surface(QXLInstance *instance, uint32_t surface_id,
                                QXLDevSurfaceCreate *surface)
{
    RedWorkerMessageCreatePrimarySurface payload = {0,};

    payload.surface_id = surface_id;
    payload.surface = *surface;
    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE, &payload);
    red_qxl_create_primary_surface_complete(instance->st, surface);
}

SPICE_GNUC_VISIBLE
void spice_qxl_reset_image_cache(QXLInstance *instance)
{
    RedWorkerMessageResetImageCache payload;

    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_RESET_IMAGE_CACHE, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_reset_cursor(QXLInstance *instance)
{
    RedWorkerMessageResetCursor payload;

    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_RESET_CURSOR, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_surface_wait(QXLInstance *instance, uint32_t surface_id)
{
    RedWorkerMessageDestroySurfaceWait payload;

    payload.surface_id = surface_id;
    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_destroy_surface_async(QXLInstance *instance, uint32_t surface_id, uint64_t cookie)
{
    RedWorkerMessageDestroySurfaceWaitAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT_ASYNC;

    payload.base.cookie = cookie;
    payload.surface_id = surface_id;
    instance->st->dispatcher->send_message(message, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_reset_memslots(QXLInstance *instance)
{
    RedWorkerMessageResetMemslots payload;

    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_RESET_MEMSLOTS, &payload);
}

static bool red_qxl_set_pending(QXLState *qxl_state, int pending)
{
    // this is not atomic but is not an issue
    if (test_bit(pending, qxl_state->pending)) {
        return TRUE;
    }

    set_bit(pending, &qxl_state->pending);
    return FALSE;
}

SPICE_GNUC_VISIBLE
void spice_qxl_wakeup(QXLInstance *instance)
{
    RedWorkerMessageWakeup payload;

    if (red_qxl_set_pending(instance->st, RED_DISPATCHER_PENDING_WAKEUP))
        return;

    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_WAKEUP, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_oom(QXLInstance *instance)
{
    RedWorkerMessageOom payload;

    if (red_qxl_set_pending(instance->st, RED_DISPATCHER_PENDING_OOM))
        return;

    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_OOM, &payload);
}

void red_qxl_start(QXLInstance *qxl)
{
    RedWorkerMessageStart payload;

    qxl->st->dispatcher->send_message(RED_WORKER_MESSAGE_START, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_flush_surfaces_async(QXLInstance *instance, uint64_t cookie)
{
    RedWorkerMessageFlushSurfacesAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_FLUSH_SURFACES_ASYNC;

    payload.base.cookie = cookie;
    instance->st->dispatcher->send_message(message, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_monitors_config_async(QXLInstance *instance, QXLPHYSICAL monitors_config,
                                     int group_id, uint64_t cookie)
{
    RedWorkerMessageMonitorsConfigAsync payload;
    RedWorkerMessage message = RED_WORKER_MESSAGE_MONITORS_CONFIG_ASYNC;

    payload.base.cookie = cookie;
    payload.monitors_config = monitors_config;
    payload.group_id = group_id;
    payload.max_monitors = instance->st->max_monitors;

    instance->st->dispatcher->send_message(message, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_driver_unload(QXLInstance *instance)
{
    RedWorkerMessageDriverUnload payload;

    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_DRIVER_UNLOAD, &payload);
}

void red_qxl_stop(QXLInstance *qxl)
{
    RedWorkerMessageStop payload;

    qxl->st->dispatcher->send_message(RED_WORKER_MESSAGE_STOP, &payload);
}

SPICE_GNUC_VISIBLE
void spice_qxl_loadvm_commands(QXLInstance *instance, struct QXLCommandExt *ext, uint32_t count)
{
    RedWorkerMessageLoadvmCommands payload;

    payload.count = count;
    payload.ext = ext;
    instance->st->dispatcher->send_message(RED_WORKER_MESSAGE_LOADVM_COMMANDS, &payload);
}

uint32_t red_qxl_get_ram_size(QXLInstance *qxl)
{
    QXLDevInitInfo qxl_info;

    red_qxl_get_init_info(qxl, &qxl_info);

    return qxl_info.qxl_ram_size;
}

SPICE_GNUC_VISIBLE
void spice_qxl_start(QXLInstance *instance)
{
    red_qxl_start(instance);
}

SPICE_GNUC_VISIBLE
void spice_qxl_stop(QXLInstance *instance)
{
    red_qxl_stop(instance);
}

SPICE_GNUC_VISIBLE
void spice_qxl_set_max_monitors(QXLInstance *instance, unsigned int max_monitors)
{
    instance->st->max_monitors = MAX(1u, max_monitors);
}

SpiceMsgDisplayGlScanoutUnix *red_qxl_get_gl_scanout(QXLInstance *qxl)
{
    pthread_mutex_lock(&qxl->st->scanout_mutex);
    if (qxl->st->scanout.drm_dma_buf_fd >= 0) {
        return &qxl->st->scanout;
    }
    pthread_mutex_unlock(&qxl->st->scanout_mutex);
    return nullptr;
}

void red_qxl_put_gl_scanout(QXLInstance *qxl, SpiceMsgDisplayGlScanoutUnix *scanout)
{
    if (scanout) {
        pthread_mutex_unlock(&qxl->st->scanout_mutex);
    }
}

SPICE_GNUC_VISIBLE
void spice_qxl_gl_scanout(QXLInstance *qxl,
                          int fd,
                          uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t format,
                          int y_0_top)
{
    RedWorkerMessageGlScanout payload = { /* empty */ };
    spice_return_if_fail(qxl != nullptr);

    QXLState *qxl_state = qxl->st;
    spice_return_if_fail(qxl_state->gl_draw_cookie == GL_DRAW_COOKIE_INVALID);

    pthread_mutex_lock(&qxl_state->scanout_mutex);

    if (qxl_state->scanout.drm_dma_buf_fd >= 0) {
        close(qxl_state->scanout.drm_dma_buf_fd);
    }

    qxl_state->scanout = (SpiceMsgDisplayGlScanoutUnix) {
        .drm_dma_buf_fd = fd,
        .width = width,
        .height = height,
        .stride = stride,
        .drm_fourcc_format = format,
        .flags = y_0_top ? SPICE_GL_SCANOUT_FLAGS_Y0TOP : 0,
    };

    pthread_mutex_unlock(&qxl_state->scanout_mutex);

    /* FIXME: find a way to coallesce all pending SCANOUTs */
    qxl_state->dispatcher->send_message(RED_WORKER_MESSAGE_GL_SCANOUT, &payload);
    reds_update_client_mouse_allowed(qxl_state->reds);
}

SPICE_GNUC_VISIBLE
void spice_qxl_gl_draw_async(QXLInstance *qxl,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h,
                             uint64_t cookie)
{
    QXLState *qxl_state;
    RedWorkerMessage message = RED_WORKER_MESSAGE_GL_DRAW_ASYNC;
    RedWorkerMessageGlDraw draw = {
        {
            .x = x,
            .y = y,
            .w = w,
            .h = h
        },
    };

    spice_return_if_fail(qxl != nullptr);
    qxl_state = qxl->st;
    if (qxl_state->scanout.drm_dma_buf_fd < 0) {
        spice_warning("called spice_qxl_gl_draw_async without a buffer");
        red_qxl_async_complete(qxl, cookie);
        return;
    }
    spice_return_if_fail(qxl_state->gl_draw_cookie == GL_DRAW_COOKIE_INVALID);

    qxl_state->gl_draw_cookie = cookie;
    qxl_state->dispatcher->send_message(message, &draw);
}

void red_qxl_gl_draw_async_complete(QXLInstance *qxl)
{
    /* this reset before usage prevent a possible race condition */
    uint64_t cookie = qxl->st->gl_draw_cookie;
    qxl->st->gl_draw_cookie = GL_DRAW_COOKIE_INVALID;
    red_qxl_async_complete(qxl, cookie);
}

SPICE_GNUC_VISIBLE
void spice_qxl_set_device_info(QXLInstance *instance,
                               const char *device_address,
                               uint32_t device_display_id_start,
                               uint32_t device_display_id_count)
{
    g_return_if_fail(device_address != nullptr);

    size_t da_len = strnlen(device_address, MAX_DEVICE_ADDRESS_LEN);
    if (da_len >= MAX_DEVICE_ADDRESS_LEN) {
        spice_error("Device address too long: %" G_GSIZE_FORMAT " > %u",
                    da_len, MAX_DEVICE_ADDRESS_LEN);
        return;
    }

    if (device_display_id_count > MAX_MONITORS_COUNT) {
        spice_error("Device display ID count (%u) is greater than limit %u",
                    device_display_id_count,
                    MAX_MONITORS_COUNT);
        return;
    }

    g_strlcpy(instance->st->device_address, device_address, MAX_DEVICE_ADDRESS_LEN);

    g_debug("QXL Instance %d setting device address \"%s\" and monitor -> device display mapping:",
            instance->id,
            device_address);

    // store the mapping monitor_id -> device_display_id
    for (uint32_t monitor_id = 0; monitor_id < device_display_id_count; ++monitor_id) {
        uint32_t device_display_id = device_display_id_start + monitor_id;
        instance->st->device_display_ids[monitor_id] = device_display_id;
        g_debug("   monitor ID %u -> device display ID %u",
                monitor_id, device_display_id);
    }

    instance->st->monitors_count = device_display_id_count;
    instance->st->max_monitors = MAX(1u, device_display_id_count);

    reds_send_device_display_info(red_qxl_get_server(instance->st));
}

uint32_t red_qxl_marshall_device_display_info(const QXLInstance *qxl, SpiceMarshaller *m)
{
    const QXLState *qxl_state = qxl->st;
    uint32_t device_count = 0;
    const char *const device_address = qxl_state->device_address;
    const size_t device_address_len = strlen(device_address) + 1;

    if (device_address_len == 1) {
        return 0;
    }
    for (size_t i = 0; i < qxl_state->monitors_count; ++i) {
        spice_marshaller_add_uint32(m, qxl->id);
        spice_marshaller_add_uint32(m, i);
        spice_marshaller_add_uint32(m, qxl_state->device_display_ids[i]);
        spice_marshaller_add_uint32(m, device_address_len);
        spice_marshaller_add(m, (const uint8_t*) (void*) device_address, device_address_len);
        ++device_count;

        g_debug("   (qxl)    channel_id: %u monitor_id: %zu, device_address: %s, "
                "device_display_id: %u",
                qxl->id, i, device_address,
                qxl_state->device_display_ids[i]);
    }
    return device_count;
}

void red_qxl_init(RedsState *reds, QXLInstance *qxl)
{
    QXLState *qxl_state;

    spice_return_if_fail(qxl != nullptr);

    qxl_state = new QXLState();
    qxl_state->reds = reds;
    qxl_state->qxl = qxl;
    pthread_mutex_init(&qxl_state->scanout_mutex, nullptr);
    qxl_state->scanout.drm_dma_buf_fd = -1;
    qxl_state->gl_draw_cookie = GL_DRAW_COOKIE_INVALID;
    qxl_state->dispatcher = red::make_shared<Dispatcher>(RED_WORKER_MESSAGE_COUNT);

    qxl_state->max_monitors = UINT_MAX;
    qxl->st = qxl_state;

    qxl_state->worker = red_worker_new(qxl);

    red_worker_run(qxl_state->worker);
}

void red_qxl_destroy(QXLInstance *qxl)
{
    spice_return_if_fail(qxl->st != nullptr && qxl->st->dispatcher);

    QXLState *qxl_state = qxl->st;

    /* send message to close thread */
    RedWorkerMessageClose message;
    qxl_state->dispatcher->send_message(RED_WORKER_MESSAGE_CLOSE_WORKER, &message);
    red_worker_free(qxl_state->worker);
    /* this must be done after calling red_worker_free */
    qxl->st = nullptr;
    pthread_mutex_destroy(&qxl_state->scanout_mutex);
    delete qxl_state;
}

Dispatcher *red_qxl_get_dispatcher(QXLInstance *qxl)
{
    return qxl->st->dispatcher.get();
}

void red_qxl_clear_pending(QXLState *qxl_state, int pending)
{
    spice_return_if_fail(qxl_state != nullptr);

    clear_bit(pending, &qxl_state->pending);
}

bool red_qxl_get_allow_client_mouse(QXLInstance *qxl, int *x_res, int *y_res, int *allow_now)
{
    // try to get resolution when 3D enabled, since qemu did not create QXL primary surface
    SpiceMsgDisplayGlScanoutUnix *gl;
    if ((gl = red_qxl_get_gl_scanout(qxl))) {
        *x_res = gl->width;
        *y_res = gl->height;
        *allow_now = TRUE;
        red_qxl_put_gl_scanout(qxl, gl);
        return true;
    }

    // check for 2D
    if (!qxl->st->primary_active) {
        return false;
    }

    if (qxl->st->use_hardware_cursor) {
        *x_res = qxl->st->x_res;
        *y_res = qxl->st->y_res;
    }
    *allow_now = qxl->st->use_hardware_cursor;
    return true;
}

void red_qxl_on_ic_change(QXLInstance *qxl, SpiceImageCompression ic)
{
    RedWorkerMessageSetCompression payload;
    payload.image_compression = ic;
    qxl->st->dispatcher->send_message(RED_WORKER_MESSAGE_SET_COMPRESSION, &payload);
}

void red_qxl_on_sv_change(QXLInstance *qxl, int sv)
{
    RedWorkerMessageSetStreamingVideo payload;
    payload.streaming_video = sv;
    qxl->st->dispatcher->send_message(RED_WORKER_MESSAGE_SET_STREAMING_VIDEO, &payload);
}

void red_qxl_on_vc_change(QXLInstance *qxl, GArray *video_codecs)
{
    RedWorkerMessageSetVideoCodecs payload;
    payload.video_codecs = g_array_ref(video_codecs);
    qxl->st->dispatcher->send_message(RED_WORKER_MESSAGE_SET_VIDEO_CODECS, &payload);
}

void red_qxl_set_mouse_mode(QXLInstance *qxl, uint32_t mode)
{
    RedWorkerMessageSetMouseMode payload;
    payload.mode = mode;
    qxl->st->dispatcher->send_message(RED_WORKER_MESSAGE_SET_MOUSE_MODE, &payload);
}

RedsState* red_qxl_get_server(QXLState *qxl_state)
{
    return qxl_state->reds;
}

void red_qxl_attach_worker(QXLInstance *qxl)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    /* cast for compatibility with spice_replay_next_cmd
     * In the past spice_replay_next_cmd received a QXLWorker instead of
     * a QXLInstance. Users of this function could have retrieved this pointer
     * only by attache_worker callback but this structure was all deprecated.
     * Passing QXLInstance pointer instead allows these programs to keep working
     * although spice_replay_next_cmd declaration changed */
    if (qxl_interface->attache_worker) {
        qxl_interface->attache_worker(qxl, (QXLWorker *) qxl);
    }
}

void red_qxl_set_compression_level(QXLInstance *qxl, int level)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);
    qxl_interface->set_compression_level(qxl, level);
}

void red_qxl_get_init_info(QXLInstance *qxl, QXLDevInitInfo *info)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    qxl_interface->get_init_info(qxl, info);
}

int red_qxl_get_command(QXLInstance *qxl, struct QXLCommandExt *cmd)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    return qxl_interface->get_command(qxl, cmd);
}

int red_qxl_req_cmd_notification(QXLInstance *qxl)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    return qxl_interface->req_cmd_notification(qxl);
}

void red_qxl_release_resource(QXLInstance *qxl, struct QXLReleaseInfoExt release_info)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    qxl_interface->release_resource(qxl, release_info);
}

int red_qxl_get_cursor_command(QXLInstance *qxl, struct QXLCommandExt *cmd)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    return qxl_interface->get_cursor_command(qxl, cmd);
}

int red_qxl_req_cursor_notification(QXLInstance *qxl)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    return qxl_interface->req_cursor_notification(qxl);
}

void red_qxl_notify_update(QXLInstance *qxl, uint32_t update_id)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    qxl_interface->notify_update(qxl, update_id);
}

int red_qxl_flush_resources(QXLInstance *qxl)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    return qxl_interface->flush_resources(qxl);
}

void red_qxl_update_area_complete(QXLInstance *qxl, uint32_t surface_id,
                                  struct QXLRect *updated_rects,
                                  uint32_t num_updated_rects)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    qxl_interface->update_area_complete(qxl, surface_id, updated_rects, num_updated_rects);
}

void red_qxl_set_client_capabilities(QXLInstance *qxl,
                                     uint8_t client_present,
                                     uint8_t caps[SPICE_CAPABILITIES_SIZE])
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    if (qxl->st->running) {
        qxl_interface->set_client_capabilities(qxl, client_present, caps);
    }
}

void red_qxl_async_complete(QXLInstance *qxl, uint64_t cookie)
{
    QXLInterface *qxl_interface = qxl_get_interface(qxl);

    qxl_interface->async_complete(qxl, cookie);
}
