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

#ifndef RED_QXL_H_
#define RED_QXL_H_

#include "red-channel.h"
#include "spice-qxl.h"

void red_qxl_init(SpiceServer *reds, QXLInstance *qxl);
void red_qxl_destroy(QXLInstance *qxl);

void red_qxl_on_ic_change(QXLInstance *qxl, SpiceImageCompression ic);
void red_qxl_on_sv_change(QXLInstance *qxl, int sv);
void red_qxl_on_vc_change(QXLInstance *qxl, GArray* video_codecs);
void red_qxl_set_mouse_mode(QXLInstance *qxl, uint32_t mode);
void red_qxl_attach_worker(QXLInstance *qxl);
void red_qxl_set_compression_level(QXLInstance *qxl, int level);
void red_qxl_stop(QXLInstance *qxl);
void red_qxl_start(QXLInstance *qxl);
uint32_t red_qxl_get_ram_size(QXLInstance *qxl);
gboolean red_qxl_client_monitors_config(QXLInstance *qxl, VDAgentMonitorsConfig *monitors_config);
bool red_qxl_get_allow_client_mouse(QXLInstance *qxl, int *x_res, int *y_res, int *allow_now);
SpiceMsgDisplayGlScanoutUnix *red_qxl_get_gl_scanout(QXLInstance *qxl);
void red_qxl_put_gl_scanout(QXLInstance *qxl, SpiceMsgDisplayGlScanoutUnix *scanout);
void red_qxl_gl_draw_async_complete(QXLInstance *qxl);
int red_qxl_check_qxl_version(QXLInstance *qxl, int major, int minor);
SpiceServer* red_qxl_get_server(QXLState *qxl);
const char* red_qxl_get_device_address(const QXLInstance *qxl);
const uint32_t* red_qxl_get_device_display_ids(const QXLInstance *qxl);
size_t red_qxl_get_monitors_count(const QXLInstance *qxl);

/* Wrappers around QXLInterface vfuncs */
void red_qxl_get_init_info(QXLInstance *qxl, QXLDevInitInfo *info);
int red_qxl_get_command(QXLInstance *qxl, struct QXLCommandExt *cmd);
int red_qxl_req_cmd_notification(QXLInstance *qxl);
void red_qxl_release_resource(QXLInstance *qxl, struct QXLReleaseInfoExt release_info);
int red_qxl_get_cursor_command(QXLInstance *qxl, struct QXLCommandExt *cmd);
int red_qxl_req_cursor_notification(QXLInstance *qxl);
void red_qxl_notify_update(QXLInstance *qxl, uint32_t update_id);
int red_qxl_flush_resources(QXLInstance *qxl);
void red_qxl_async_complete(QXLInstance *qxl, uint64_t cookie);
void red_qxl_update_area_complete(QXLInstance *qxl, uint32_t surface_id,
                                  struct QXLRect *updated_rects,
                                  uint32_t num_updated_rects);
void red_qxl_set_client_capabilities(QXLInstance *qxl,
                                     uint8_t client_present,
                                     uint8_t caps[SPICE_CAPABILITIES_SIZE]);

#endif /* RED_QXL_H_ */
