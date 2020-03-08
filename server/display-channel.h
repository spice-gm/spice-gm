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

#ifndef DISPLAY_CHANNEL_H_
# define DISPLAY_CHANNEL_H_

#include <setjmp.h>
#include <common/rect.h>

#include "reds.h"
#include "red-parse-qxl.h"
#include "red-channel.h"
#include "main-channel.h"
#include "spice-bitmap-utils.h"
#include "tree.h"
#include "video-stream.h"
#include "dcc.h"
#include "image-encoders.h"
#include "common-graphics-channel.h"
#include "utils.hpp"

#include "push-visibility.h"

struct DisplayChannelPrivate;

struct DisplayChannel final: public CommonGraphicsChannel
{
    DisplayChannel(RedsState *reds,
                   QXLInstance *qxl,
                   SpiceCoreInterfaceInternal *core,
                   Dispatcher *dispatcher,
                   int migrate, int stream_video,
                   GArray *video_codecs,
                   uint32_t n_surfaces);
    ~DisplayChannel();
    void on_connect(RedClient *client, RedStream *stream, int migration,
                    RedChannelCapabilities *caps) override;
    red::unique_link<DisplayChannelPrivate> priv;
};

typedef struct DependItem {
    Drawable *drawable;
    RingItem ring_item;
} DependItem;

struct Drawable {
    uint32_t refs;
    RingItem surface_list_link;
    RingItem list_link;
    DrawItem tree_item;
    GList *pipes;
    RedDrawable *red_drawable;

    GlzImageRetention glz_retention;

    red_time_t creation_time;
    red_time_t first_frame_time;
    int frames_count;
    int gradual_frames_count;
    int last_gradual_frame;
    VideoStream *stream;
    int streamable;
    BitmapGradualType copy_bitmap_graduality;
    DependItem depend_items[3];

    int surface_id;
    int surface_deps[3];

    uint32_t process_commands_generation;
    DisplayChannel *display;
};

red::shared_ptr<DisplayChannel>
display_channel_new(RedsState *reds, QXLInstance *qxl,
                    SpiceCoreInterfaceInternal *core, Dispatcher *dispatcher,
                    int migrate, int stream_video,
                    GArray *video_codecs,
                    uint32_t n_surfaces);
void                       display_channel_create_surface            (DisplayChannel *display, uint32_t surface_id,
                                                                      uint32_t width, uint32_t height,
                                                                      int32_t stride, uint32_t format, void *line_0,
                                                                      int data_is_valid, int send_client);
void                       display_channel_draw                      (DisplayChannel *display,
                                                                      const SpiceRect *area,
                                                                      int surface_id);
void                       display_channel_update                    (DisplayChannel *display,
                                                                      uint32_t surface_id,
                                                                      const QXLRect *area,
                                                                      uint32_t clear_dirty,
                                                                      QXLRect **qxl_dirty_rects,
                                                                      uint32_t *num_dirty_rects);
void                       display_channel_free_some                 (DisplayChannel *display);
void                       display_channel_set_stream_video          (DisplayChannel *display,
                                                                      int stream_video);
void                       display_channel_set_video_codecs          (DisplayChannel *display,
                                                                      GArray *video_codecs);
int                        display_channel_get_streams_timeout       (DisplayChannel *display);
void                       display_channel_compress_stats_print      (DisplayChannel *display);
void                       display_channel_compress_stats_reset      (DisplayChannel *display);
void                       display_channel_surface_unref             (DisplayChannel *display,
                                                                      uint32_t surface_id);
bool                       display_channel_wait_for_migrate_data     (DisplayChannel *display);
void                       display_channel_flush_all_surfaces        (DisplayChannel *display);
void                       display_channel_free_glz_drawables_to_free(DisplayChannel *display);
void                       display_channel_free_glz_drawables        (DisplayChannel *display);
void                       display_channel_destroy_surface_wait      (DisplayChannel *display,
                                                                      uint32_t surface_id);
void                       display_channel_destroy_surfaces          (DisplayChannel *display);
void                       display_channel_process_draw              (DisplayChannel *display,
                                                                      RedDrawable *red_drawable,
                                                                      uint32_t process_commands_generation);
void                       display_channel_process_surface_cmd       (DisplayChannel *display,
                                                                      RedSurfaceCmd *surface_cmd,
                                                                      int loadvm);
void                       display_channel_gl_scanout                (DisplayChannel *display);
void                       display_channel_gl_draw                   (DisplayChannel *display,
                                                                      SpiceMsgDisplayGlDraw *draw);
void                       display_channel_gl_draw_done              (DisplayChannel *display);

void display_channel_update_monitors_config(DisplayChannel *display, const QXLMonitorsConfig *config,
                                            uint16_t count, uint16_t max_allowed);
void display_channel_set_monitors_config_to_primary(DisplayChannel *display);
void display_channel_push_monitors_config(DisplayChannel *display);

gboolean display_channel_validate_surface(DisplayChannel *display, uint32_t surface_id);
gboolean display_channel_surface_has_canvas(DisplayChannel *display, uint32_t surface_id);
void display_channel_reset_image_cache(DisplayChannel *self);

void display_channel_debug_oom(DisplayChannel *display, const char *msg);

void display_channel_update_qxl_running(DisplayChannel *display, bool running);
void display_channel_set_image_compression(DisplayChannel *display,
                                           SpiceImageCompression image_compression);

#include "pop-visibility.h"

#endif /* DISPLAY_CHANNEL_H_ */
