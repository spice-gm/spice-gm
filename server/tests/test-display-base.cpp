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
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <sys/select.h>
#endif
#include <sys/types.h>
#include <getopt.h>
#include <pthread.h>

#include "spice-wrapped.h"
#include <spice/qxl_dev.h>

#include "test-display-base.h"
#include "test-glib-compat.h"
#include "red-channel.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MEM_SLOT_GROUP_ID 0

#define NOTIFY_DISPLAY_BATCH (SINGLE_PART/2)
#define NOTIFY_CURSOR_BATCH 10

/* Parts cribbed from spice-display.h/.c/qxl.c */

struct SimpleSpiceUpdate {
    QXLCommandExt ext; // first
    QXLDrawable drawable;
    QXLImage image;
    uint8_t *bitmap;
};

struct SimpleSurfaceCmd {
    QXLCommandExt ext; // first
    QXLSurfaceCmd surface_cmd;
};

static void test_spice_destroy_update(SimpleSpiceUpdate *update)
{
    if (!update) {
        return;
    }
    if (update->drawable.clip.type != SPICE_CLIP_TYPE_NONE) {
        auto ptr = (uint8_t*)(uintptr_t)update->drawable.clip.data;
        g_free(ptr);
    }
    g_free(update->bitmap);
    g_free(update);
}

#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 320

#define SINGLE_PART 4
static const int angle_parts = 64 / SINGLE_PART;
static int unique = 1;
static int color = -1;
static int c_i = 0;

/* Used for automated tests */
static int control = 3; //used to know when we can take a screenshot
static int rects = 16; //number of rects that will be draw
static int has_automated_tests = 0; //automated test flag

// SPICE implementation is not designed to have a timer shared
// between multiple threads so use a mutex
static pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;

// wait for the child process and exit
static void child_exited(GPid pid, gint status, gpointer user_data)
{
    g_spawn_close_pid(pid);
    exit(0);
}

static void regression_test()
{
    GPid pid;
    GError *error = nullptr;
    gboolean retval;
    gchar **argv;

    if (--rects != 0) {
        return;
    }

    rects = 16;

    if (--control != 0) {
        return;
    }

    argv = g_strsplit("./regression-test.py", " ", -1);
    retval = g_spawn_async(nullptr, argv, nullptr, (GSpawnFlags) (G_SPAWN_SEARCH_PATH|G_SPAWN_DO_NOT_REAP_CHILD),
                           nullptr, nullptr, &pid, &error);
    g_strfreev(argv);
    g_assert(retval);

    GSource *source = g_child_watch_source_new(pid);
    g_source_set_callback(source, (GSourceFunc)(void*)child_exited, nullptr, nullptr);
    guint id = g_source_attach(source, basic_event_loop_get_context());
    g_assert(id != 0);
    g_source_unref(source);
}

static void set_cmd(QXLCommandExt *ext, uint32_t type, QXLPHYSICAL data)
{
    ext->cmd.type = type;
    ext->cmd.data = data;
    ext->cmd.padding = 0;
    ext->group_id = MEM_SLOT_GROUP_ID;
    ext->flags = 0;
}

static void simple_set_release_info(QXLReleaseInfo *info, intptr_t ptr)
{
    info->id = ptr;
    //info->group_id = MEM_SLOT_GROUP_ID;
}

struct Path {
    int t;
    int min_t;
    int max_t;
};

static void path_init(Path *path, int min, int max)
{
    path->t = min;
    path->min_t = min;
    path->max_t = max;
}

static void path_progress(Path *path)
{
    path->t = (path->t+1)% (path->max_t - path->min_t) + path->min_t;
}

Path path;

static void draw_pos(Test *test, int t, int *x, int *y)
{
#ifdef CIRCLE
    *y = test->primary_height/2 + (test->primary_height/3)*cos(t*2*M_PI/angle_parts);
    *x = test->primary_width/2 + (test->primary_width/3)*sin(t*2*M_PI/angle_parts);
#else
    *y = test->primary_height*(t % SINGLE_PART)/SINGLE_PART;
    *x = ((test->primary_width/SINGLE_PART)*(t / SINGLE_PART)) % test->primary_width;
#endif
}

/* bitmap and rects are freed, so they must be allocated with malloc */
static SimpleSpiceUpdate *
test_spice_create_update_from_bitmap(uint32_t surface_id,
                                     QXLRect bbox,
                                     uint8_t *bitmap,
                                     uint32_t num_clip_rects,
                                     QXLRect *clip_rects)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    QXLImage *image;
    uint32_t bw, bh;

    bh = bbox.bottom - bbox.top;
    bw = bbox.right - bbox.left;

    update   = g_new0(SimpleSpiceUpdate, 1);
    update->bitmap = bitmap;
    drawable = &update->drawable;
    image    = &update->image;

    drawable->surface_id      = surface_id;

    drawable->bbox            = bbox;
    if (num_clip_rects == 0) {
        drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    } else {
        QXLClipRects *cmd_clip;

        cmd_clip = (QXLClipRects*) g_malloc0(sizeof(QXLClipRects) + num_clip_rects*sizeof(QXLRect));
        cmd_clip->num_rects = num_clip_rects;
        cmd_clip->chunk.data_size = num_clip_rects*sizeof(QXLRect);
        cmd_clip->chunk.prev_chunk = cmd_clip->chunk.next_chunk = 0;
        memcpy(cmd_clip + 1, clip_rects, cmd_clip->chunk.data_size);

        drawable->clip.type = SPICE_CLIP_TYPE_RECTS;
        drawable->clip.data = (intptr_t)cmd_clip;

        g_free(clip_rects);
    }
    drawable->effect          = QXL_EFFECT_OPAQUE;
    simple_set_release_info(&drawable->release_info, (intptr_t)update);
    drawable->type            = QXL_DRAW_COPY;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    drawable->u.copy.rop_descriptor  = SPICE_ROPD_OP_PUT;
    drawable->u.copy.src_bitmap      = (intptr_t)image;
    drawable->u.copy.src_area.right  = bw;
    drawable->u.copy.src_area.bottom = bh;

    QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_DEVICE, unique);
    image->descriptor.type   = SPICE_IMAGE_TYPE_BITMAP;
    image->bitmap.flags      = QXL_BITMAP_DIRECT | QXL_BITMAP_TOP_DOWN;
    image->bitmap.stride     = bw * 4;
    image->descriptor.width  = image->bitmap.x = bw;
    image->descriptor.height = image->bitmap.y = bh;
    image->bitmap.data = (intptr_t)bitmap;
    image->bitmap.palette = 0;
    image->bitmap.format = SPICE_BITMAP_FMT_32BIT;

    set_cmd(&update->ext, QXL_CMD_DRAW, (intptr_t)drawable);

    return update;
}

static SimpleSpiceUpdate *test_spice_create_update_solid(uint32_t surface_id, QXLRect bbox,
                                                         uint32_t solid_color)
{
    uint8_t *bitmap;
    uint32_t *dst;
    uint32_t bw;
    uint32_t bh;
    uint32_t i;

    bw = bbox.right - bbox.left;
    bh = bbox.bottom - bbox.top;

    bitmap = (uint8_t*) g_malloc(bw * bh * 4);
    dst = SPICE_ALIGNED_CAST(uint32_t *, bitmap);

    for (i = 0 ; i < bh * bw ; ++i, ++dst) {
        *dst = solid_color;
    }

    return test_spice_create_update_from_bitmap(surface_id, bbox, bitmap, 0, nullptr);
}

static SimpleSpiceUpdate *test_spice_create_update_draw(Test *test, uint32_t surface_id, int t)
{
    int top, left;
    uint8_t *dst;
    uint8_t *bitmap;
    int bw, bh;
    int i;
    QXLRect bbox;

    draw_pos(test, t, &left, &top);
    if ((t % angle_parts) == 0) {
        c_i++;
    }

    if (surface_id != 0) {
        color = (color + 1) % 2;
    } else {
        color = surface_id;
    }

    unique++;

    bw       = test->primary_width/SINGLE_PART;
    bh       = 48;

    bitmap = dst = (uint8_t*) g_malloc(bw * bh * 4);
    //printf("allocated %p\n", dst);

    for (i = 0 ; i < bh * bw ; ++i, dst+=4) {
        *dst = (color+i % 255);
        *(dst+((1+c_i)%3)) = 255 - color;
        *(dst+((2+c_i)%3)) = (color * (color + i)) & 0xff;
        *(dst+((3+c_i)%3)) = 0;
    }

    bbox.left = left; bbox.top = top;
    bbox.right = left + bw; bbox.bottom = top + bh;
    return test_spice_create_update_from_bitmap(surface_id, bbox, bitmap, 0, nullptr);
}

static SimpleSpiceUpdate *test_spice_create_update_copy_bits(Test *test, uint32_t surface_id)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    int bw, bh;
    QXLRect bbox = {
        .top = 0,
        .left = 10,
    };

    update   = g_new0(SimpleSpiceUpdate, 1);
    drawable = &update->drawable;

    bw       = test->primary_width/SINGLE_PART;
    bh       = 48;
    bbox.right = bbox.left + bw;
    bbox.bottom = bbox.top + bh;
    //printf("allocated %p, %p\n", update, update->bitmap);

    drawable->surface_id      = surface_id;

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    simple_set_release_info(&drawable->release_info, (intptr_t)update);
    drawable->type            = QXL_COPY_BITS;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    drawable->u.copy_bits.src_pos.x = 0;
    drawable->u.copy_bits.src_pos.y = 0;

    set_cmd(&update->ext, QXL_CMD_DRAW, (intptr_t)drawable);

    return update;
}

static int format_to_bpp(int format)
{
    switch (format) {
    case SPICE_SURFACE_FMT_8_A:
        return 1;
    case SPICE_SURFACE_FMT_16_555:
    case SPICE_SURFACE_FMT_16_565:
        return 2;
    case SPICE_SURFACE_FMT_32_xRGB:
    case SPICE_SURFACE_FMT_32_ARGB:
        return 4;
    }
    abort();
}

static SimpleSurfaceCmd *create_surface(int surface_id, int format, int width, int height, uint8_t *data)
{
    auto simple_cmd = g_new0(SimpleSurfaceCmd, 1);
    QXLSurfaceCmd *surface_cmd = &simple_cmd->surface_cmd;
    int bpp = format_to_bpp(format);

    set_cmd(&simple_cmd->ext, QXL_CMD_SURFACE, (intptr_t)surface_cmd);
    simple_set_release_info(&surface_cmd->release_info, (intptr_t)simple_cmd);
    surface_cmd->type = QXL_SURFACE_CMD_CREATE;
    surface_cmd->flags = 0; // ?
    surface_cmd->surface_id = surface_id;
    surface_cmd->u.surface_create.format = format;
    surface_cmd->u.surface_create.width = width;
    surface_cmd->u.surface_create.height = height;
    surface_cmd->u.surface_create.stride = -width * bpp;
    surface_cmd->u.surface_create.data = (intptr_t)data;
    return simple_cmd;
}

static SimpleSurfaceCmd *destroy_surface(int surface_id)
{
    auto simple_cmd = g_new0(SimpleSurfaceCmd, 1);
    QXLSurfaceCmd *surface_cmd = &simple_cmd->surface_cmd;

    set_cmd(&simple_cmd->ext, QXL_CMD_SURFACE, (intptr_t)surface_cmd);
    simple_set_release_info(&surface_cmd->release_info, (intptr_t)simple_cmd);
    surface_cmd->type = QXL_SURFACE_CMD_DESTROY;
    surface_cmd->flags = 0; // ?
    surface_cmd->surface_id = surface_id;
    return simple_cmd;
}

static void create_primary_surface(Test *test, uint32_t width,
                                   uint32_t height)
{
    QXLDevSurfaceCreate surface = { 0, };

    spice_assert(height <= MAX_HEIGHT);
    spice_assert(width <= MAX_WIDTH);
    spice_assert(height > 0);
    spice_assert(width > 0);

    surface.format     = SPICE_SURFACE_FMT_32_xRGB;
    surface.width      = test->primary_width = width;
    surface.height     = test->primary_height = height;
    surface.stride     = -width * 4; /* negative? */
    surface.mouse_mode = TRUE; /* unused by red_worker */
    surface.flags      = 0;
    surface.type       = 0;    /* unused by red_worker */
    surface.position   = 0;    /* unused by red_worker */
    surface.mem        = (uintptr_t)&test->primary_surface;
    surface.group_id   = MEM_SLOT_GROUP_ID;

    test->width = width;
    test->height = height;

    spice_qxl_create_primary_surface(&test->qxl_instance, 0, &surface);
}

static QXLDevMemSlot slot = {
.slot_group_id = MEM_SLOT_GROUP_ID,
.slot_id = 0,
.generation = 0,
.virt_start = 0,
.virt_end = ~0,
.addr_delta = 0,
.qxl_ram_size = ~0,
};

static void attached_worker(QXLInstance *qin)
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);

    printf("%s\n", __func__);
    spice_qxl_add_memslot(&test->qxl_instance, &slot);
    create_primary_surface(test, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    spice_server_vm_start(test->server);
}

static void set_compression_level(SPICE_GNUC_UNUSED QXLInstance *qin,
                                  SPICE_GNUC_UNUSED int level)
{
    printf("%s\n", __func__);
}

// we now have a secondary surface
#define MAX_SURFACE_NUM 2

static void get_init_info(SPICE_GNUC_UNUSED QXLInstance *qin,
                          QXLDevInitInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->num_memslots = 1;
    info->num_memslots_groups = 1;
    info->memslot_id_bits = 1;
    info->memslot_gen_bits = 1;
    info->n_surfaces = MAX_SURFACE_NUM;
}

// We shall now have a ring of commands, so that we can update
// it from a separate thread - since get_command is called from
// the worker thread, and we need to sometimes do an update_area,
// which cannot be done from red_worker context (not via dispatcher,
// since you get a deadlock, and it isn't designed to be done
// any other way, so no point testing that).
static unsigned int commands_end = 0;
static unsigned int commands_start = 0;
static struct QXLCommandExt* commands[1024];
static pthread_mutex_t command_mutex = PTHREAD_MUTEX_INITIALIZER;

#define COMMANDS_SIZE G_N_ELEMENTS(commands)

static void push_command(QXLCommandExt *ext)
{
    pthread_mutex_lock(&command_mutex);
    spice_assert(commands_end - commands_start < COMMANDS_SIZE);
    commands[commands_end % COMMANDS_SIZE] = ext;
    commands_end++;
    pthread_mutex_unlock(&command_mutex);
}

static int get_num_commands()
{
    return commands_end - commands_start;
}

static struct QXLCommandExt *get_simple_command()
{
    pthread_mutex_lock(&command_mutex);
    struct QXLCommandExt *ret = nullptr;
    if (get_num_commands() > 0) {
        ret = commands[commands_start % COMMANDS_SIZE];
        commands_start++;
    }
    pthread_mutex_unlock(&command_mutex);
    return ret;
}

// called from spice_server thread (i.e. red_worker thread)
static int get_command(SPICE_GNUC_UNUSED QXLInstance *qin,
                       struct QXLCommandExt *ext)
{
    struct QXLCommandExt *cmd = get_simple_command();
    if (!cmd) {
        return FALSE;
    }
    *ext = *cmd;
    return TRUE;
}

static void produce_command(Test *test)
{
    Command *command;

    if (test->has_secondary)
        test->target_surface = 1;

    if (!test->num_commands) {
        usleep(1000);
        return;
    }

    command = &test->commands[test->cmd_index];
    if (command->cb) {
        command->cb(test, command);
    }
    switch (command->command) {
        case SLEEP:
             printf("sleep %u seconds\n", command->sleep.secs);
             sleep(command->sleep.secs);
             break;
        case PATH_PROGRESS:
            path_progress(&path);
            break;
        case SIMPLE_UPDATE: {
            QXLRect rect = {
                .top = 0,
                .left = 0,
                .bottom = (test->target_surface == 0 ? test->primary_height : test->height),
                .right = (test->target_surface == 0 ? test->primary_width : test->width),
            };
            if (rect.right > 0 && rect.bottom > 0) {
                spice_qxl_update_area(&test->qxl_instance, test->target_surface, &rect, nullptr, 0, 1);
            }
            break;
        }

        /* Drawing commands, they all push a command to the command ring */
        case SIMPLE_COPY_BITS:
        case SIMPLE_DRAW_SOLID:
        case SIMPLE_DRAW_BITMAP:
        case SIMPLE_DRAW: {
            SimpleSpiceUpdate *update;

            if (has_automated_tests)
            {
                if (control == 0) {
                     return;
                }

                regression_test();
            }

            switch (command->command) {
            case SIMPLE_COPY_BITS:
                update = test_spice_create_update_copy_bits(test, 0);
                break;
            case SIMPLE_DRAW:
                update = test_spice_create_update_draw(test, 0, path.t);
                break;
            case SIMPLE_DRAW_BITMAP:
                update = test_spice_create_update_from_bitmap(command->bitmap.surface_id,
                        command->bitmap.bbox, command->bitmap.bitmap,
                        command->bitmap.num_clip_rects, command->bitmap.clip_rects);
                break;
            case SIMPLE_DRAW_SOLID:
                update = test_spice_create_update_solid(command->solid.surface_id,
                        command->solid.bbox, command->solid.color);
                break;
            default:
                /* Terminate on unhandled cases - never actually reached */
                g_assert_not_reached();
                break;
            }
            push_command(&update->ext);
            break;
        }

        case SIMPLE_CREATE_SURFACE: {
            SimpleSurfaceCmd *update;
            if (command->create_surface.data) {
                spice_assert(command->create_surface.surface_id > 0);
                spice_assert(command->create_surface.surface_id < MAX_SURFACE_NUM);
                spice_assert(command->create_surface.surface_id == 1);
                update = create_surface(command->create_surface.surface_id,
                                        command->create_surface.format,
                                        command->create_surface.width,
                                        command->create_surface.height,
                                        command->create_surface.data);
            } else {
                update = create_surface(test->target_surface, SPICE_SURFACE_FMT_32_xRGB,
                                        SURF_WIDTH, SURF_HEIGHT,
                                        test->secondary_surface);
            }
            push_command(&update->ext);
            test->has_secondary = 1;
            break;
        }

        case SIMPLE_DESTROY_SURFACE: {
            SimpleSurfaceCmd *update;
            test->has_secondary = 0;
            update = destroy_surface(test->target_surface);
            test->target_surface = 0;
            push_command(&update->ext);
            break;
        }

        case DESTROY_PRIMARY:
            spice_qxl_destroy_primary_surface(&test->qxl_instance, 0);
            break;

        case CREATE_PRIMARY:
            create_primary_surface(test,
                    command->create_primary.width, command->create_primary.height);
            break;
    }
    test->cmd_index = (test->cmd_index + 1) % test->num_commands;
}

static int req_cmd_notification(QXLInstance *qin)
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);

    pthread_mutex_lock(&timer_mutex);
    test->core->timer_start(test->wakeup_timer, test->wakeup_ms);
    pthread_mutex_unlock(&timer_mutex);
    return TRUE;
}

static void do_wakeup(void *opaque)
{
    auto test = (Test*) opaque;
    int notify;

    test->cursor_notify = NOTIFY_CURSOR_BATCH;
    for (notify = NOTIFY_DISPLAY_BATCH; notify > 0;--notify) {
        produce_command(test);
    }

    pthread_mutex_lock(&timer_mutex);
    test->core->timer_start(test->wakeup_timer, test->wakeup_ms);
    pthread_mutex_unlock(&timer_mutex);
    spice_qxl_wakeup(&test->qxl_instance);
}

static void release_resource(SPICE_GNUC_UNUSED QXLInstance *qin,
                             struct QXLReleaseInfoExt release_info)
{
    auto ext = (QXLCommandExt*)(uintptr_t)release_info.info->id;
    //printf("%s\n", __func__);
    spice_assert(release_info.group_id == MEM_SLOT_GROUP_ID);
    switch (ext->cmd.type) {
        case QXL_CMD_DRAW:
            test_spice_destroy_update(SPICE_CONTAINEROF(ext, SimpleSpiceUpdate, ext));
            break;
        case QXL_CMD_SURFACE:
            g_free(ext);
            break;
        case QXL_CMD_CURSOR: {
            auto cmd = (QXLCursorCmd *)(uintptr_t)ext->cmd.data;
            if (cmd->type == QXL_CURSOR_SET || cmd->type == QXL_CURSOR_MOVE) {
                g_free(cmd);
            }
            g_free(ext);
            break;
        }
        default:
            abort();
    }
}

#define CURSOR_WIDTH 32
#define CURSOR_HEIGHT 32

static struct {
    QXLCursor cursor;
    uint8_t data[CURSOR_WIDTH * CURSOR_HEIGHT * 4 + 128]; // 32bit per pixel
} cursor;

static void cursor_init()
{
    cursor.cursor.header.unique = 0;
    cursor.cursor.header.type = SPICE_CURSOR_TYPE_COLOR32;
    cursor.cursor.header.width = CURSOR_WIDTH;
    cursor.cursor.header.height = CURSOR_HEIGHT;
    cursor.cursor.header.hot_spot_x = 0;
    cursor.cursor.header.hot_spot_y = 0;
    cursor.cursor.data_size = CURSOR_WIDTH * CURSOR_HEIGHT * 4;

    // X drivers addes it to the cursor size because it could be
    // cursor data information or another cursor related stuffs.
    // Otherwise, the code will break in client/cursor.cpp side,
    // that expect the data_size plus cursor information.
    // Blame cursor protocol for this. :-)
    cursor.cursor.data_size += 128;
    cursor.cursor.chunk.data_size = cursor.cursor.data_size;
    cursor.cursor.chunk.prev_chunk = cursor.cursor.chunk.next_chunk = 0;
}

static int get_cursor_command(QXLInstance *qin, struct QXLCommandExt *ext)
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);
    static int set = 1;
    static int x = 0, y = 0;
    QXLCursorCmd *cursor_cmd;
    QXLCommandExt *cmd;

    if (!test->cursor_notify) {
        return FALSE;
    }

    test->cursor_notify--;
    cmd = g_new0(QXLCommandExt, 1);
    cursor_cmd = g_new0(QXLCursorCmd, 1);

    cursor_cmd->release_info.id = (uintptr_t)cmd;

    if (set) {
        cursor_cmd->type = QXL_CURSOR_SET;
        cursor_cmd->u.set.position.x = 0;
        cursor_cmd->u.set.position.y = 0;
        cursor_cmd->u.set.visible = TRUE;
        cursor_cmd->u.set.shape = (uintptr_t)&cursor;
        // Only a white rect (32x32) as cursor
        memset(cursor.data, 255, sizeof(cursor.data));
        set = 0;
    } else {
        cursor_cmd->type = QXL_CURSOR_MOVE;
        cursor_cmd->u.position.x = x++ % test->primary_width;
        cursor_cmd->u.position.y = y++ % test->primary_height;
    }

    cmd->cmd.data = (uintptr_t)cursor_cmd;
    cmd->cmd.type = QXL_CMD_CURSOR;
    cmd->group_id = MEM_SLOT_GROUP_ID;
    cmd->flags    = 0;
    *ext = *cmd;
    //printf("%s\n", __func__);
    return TRUE;
}

static int req_cursor_notification(SPICE_GNUC_UNUSED QXLInstance *qin)
{
    printf("%s\n", __func__);
    return TRUE;
}

static void notify_update(SPICE_GNUC_UNUSED QXLInstance *qin,
                          SPICE_GNUC_UNUSED uint32_t update_id)
{
    printf("%s\n", __func__);
}

static int flush_resources(SPICE_GNUC_UNUSED QXLInstance *qin)
{
    printf("%s\n", __func__);
    return TRUE;
}

static int client_monitors_config(SPICE_GNUC_UNUSED QXLInstance *qin,
                                  VDAgentMonitorsConfig *monitors_config)
{
    if (!monitors_config) {
        printf("%s: NULL monitors_config\n", __func__);
    } else {
        printf("%s: %d\n", __func__, monitors_config->num_of_monitors);
    }
    return 0;
}

static void set_client_capabilities(QXLInstance *qin,
                                    uint8_t client_present,
                                    uint8_t caps[58])
{
    Test *test = SPICE_CONTAINEROF(qin, Test, qxl_instance);

    printf("%s: present %d caps %d\n", __func__, client_present, caps[0]);
    if (test->on_client_connected && client_present) {
        test->on_client_connected(test);
    }
    if (test->on_client_disconnected && !client_present) {
        test->on_client_disconnected(test);
    }
}

static QXLInterface display_sif = {
    .base = {
        .type = SPICE_INTERFACE_QXL,
        .description = "test",
        .major_version = SPICE_INTERFACE_QXL_MAJOR,
        .minor_version = SPICE_INTERFACE_QXL_MINOR
    },
    { .attached_worker = attached_worker },
    .set_compression_level = set_compression_level,
    .set_mm_time = nullptr,
    .get_init_info = get_init_info,

    /* the callbacks below are called from spice server thread context */
    .get_command = get_command,
    .req_cmd_notification = req_cmd_notification,
    .release_resource = release_resource,
    .get_cursor_command = get_cursor_command,
    .req_cursor_notification = req_cursor_notification,
    .notify_update = notify_update,
    .flush_resources = flush_resources,
    .async_complete = nullptr,
    .update_area_complete = nullptr,
    .set_client_capabilities = set_client_capabilities,
    .client_monitors_config = client_monitors_config,
};

/* interface for tests */
void test_add_display_interface(Test* test)
{
    spice_server_add_interface(test->server, &test->qxl_instance.base);
}

static int vmc_write(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                     SPICE_GNUC_UNUSED const uint8_t *buf,
                     int len)
{
    printf("%s: %d\n", __func__, len);
    return len;
}

static int vmc_read(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                    SPICE_GNUC_UNUSED uint8_t *buf,
                    int len)
{
    printf("%s: %d\n", __func__, len);
    return 0;
}

static void vmc_state(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                      int connected)
{
    printf("%s: %d\n", __func__, connected);
}


static SpiceCharDeviceInterface vdagent_sif = {
    .base = {
        .type          = SPICE_INTERFACE_CHAR_DEVICE,
        .description   = "test spice virtual channel char device",
        .major_version = SPICE_INTERFACE_CHAR_DEVICE_MAJOR,
        .minor_version = SPICE_INTERFACE_CHAR_DEVICE_MINOR,
    },
    .state              = vmc_state,
    .write              = vmc_write,
    .read               = vmc_read,

};

static SpiceCharDeviceInstance vdagent_sin = {
    .base = {
        .sif = &vdagent_sif.base,
    },
    .subtype = "vdagent",
};

void test_add_agent_interface(SpiceServer *server)
{
    spice_server_add_interface(server, &vdagent_sin.base);
}

void test_set_simple_command_list(Test *test, const int *simple_commands, int num_commands)
{
    int i;

    g_free(test->commands);
    test->commands = g_new0(Command, num_commands);
    test->num_commands = num_commands;
    for (i = 0 ; i < num_commands; ++i) {
        test->commands[i].command = (CommandType) simple_commands[i];
    }
}

void test_set_command_list(Test *test, Command *new_commands, int num_commands)
{
    test->commands = new_commands;
    test->num_commands = num_commands;
}

static gboolean ignore_in_use_failures(const gchar *log_domain,
                                       GLogLevelFlags log_level,
                                       const gchar *message,
                                       gpointer user_data)
{
    if (!g_str_equal (log_domain, G_LOG_DOMAIN)) {
        return true;
    }
    if ((log_level & G_LOG_LEVEL_WARNING) == 0)  {
        return true;
    }
    if (strstr(message, "reds_init_socket: binding socket to ") == nullptr && // bind failure
        strstr(message, "reds_init_socket: listen: ") == nullptr && // listen failure
        strstr(message, "Failed to open SPICE sockets") == nullptr) { // global
        g_print("XXX [%s]\n", message);
        return true;
    }

    return false;
}

#define BASE_PORT 5912

Test* test_new(SpiceCoreInterface* core)
{
    auto test = g_new0(Test, 1);
    int port = -1;

    test->qxl_instance.base.sif = &display_sif.base;
    test->qxl_instance.id = 0;

    test->core = core;
    test->wakeup_ms = 1;
    test->cursor_notify = NOTIFY_CURSOR_BATCH;
    // some common initialization for all display tests
    port = BASE_PORT;

    g_test_log_set_fatal_handler(ignore_in_use_failures, nullptr);
    for (port = BASE_PORT; port < BASE_PORT + 10; port++) {
        SpiceServer* server = spice_server_new();
        spice_server_set_noauth(server);
        spice_server_set_port(server, port);
        if (spice_server_init(server, core) == 0) {
            test->server = server;
            break;
        }
        spice_server_destroy(server);
    }

    g_assert_nonnull(test->server);

    printf("TESTER: listening on port %d (unsecure)\n", port);
    g_test_log_set_fatal_handler(nullptr, nullptr);

    cursor_init();
    path_init(&path, 0, angle_parts);
    test->has_secondary = 0;
    test->wakeup_timer = core->timer_add(do_wakeup, test);
    return test;
}

void test_destroy(Test *test)
{
    spice_server_destroy(test->server);
    // this timer is used by spice server so
    // avoid to free it while is running
    test->core->timer_remove(test->wakeup_timer);
    g_free(test->commands);
    g_free(test);
}

static void init_automated()
{
}

static SPICE_GNUC_NORETURN
void usage(const char *argv0, const int exitcode)
{
    const char *autoopt=" [--automated-tests]";

    printf("usage: %s%s\n", argv0, autoopt);
    exit(exitcode);
}

void spice_test_config_parse_args(int argc, char **argv)
{
    struct option options[] = {
        {"automated-tests", no_argument, &has_automated_tests, 1},
        {nullptr, 0, nullptr, 0},
    };
    int option_index;
    int val;

    while ((val = getopt_long(argc, argv, "", options, &option_index)) != -1) {
        switch (val) {
        case '?':
            printf("unrecognized option '%s'\n", argv[optind - 1]);
            usage(argv[0], EXIT_FAILURE);
        case 0:
            break;
        }
    }

    if (argc > optind) {
        printf("unknown argument '%s'\n", argv[optind]);
        usage(argv[0], EXIT_FAILURE);
    }
    if (has_automated_tests) {
        init_automated();
    }
    return;
}
