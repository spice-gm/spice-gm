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
/**
 * Recreate the primary surface endlessly.
 */

#include <config.h>
#include <math.h>
#include <stdlib.h>
#include "test-display-base.h"

static SpiceTimer *ping_timer;

static int ping_ms = 100;

static void pinger(void *opaque)
{
    Test *test = (Test*) opaque;

    test->core->timer_start(ping_timer, ping_ms);
}

static int g_surface_id = 1;
static uint8_t *g_surface_data;

static void
set_draw_parameters(SPICE_GNUC_UNUSED Test *test, Command *command)
{
    static int count = 17;
    CommandDrawSolid *solid = &command->solid;

    solid->bbox.top = 0;
    solid->bbox.left = 0;
    solid->bbox.bottom = 20;
    solid->bbox.right = count;
    solid->surface_id = g_surface_id;
    count++;
}

static void
set_surface_params(SPICE_GNUC_UNUSED Test *test, Command *command)
{
    CommandCreateSurface *create = &command->create_surface;

    // UGLY
    if (g_surface_data) {
        exit(0);
    }
    create->format = SPICE_SURFACE_FMT_8_A;
    create->width = 128;
    create->height = 128;
    g_surface_data = (uint8_t*) g_realloc(g_surface_data, create->width * create->height * 1);
    create->surface_id = g_surface_id;
    create->data = g_surface_data;
}

static void
set_destroy_parameters(SPICE_GNUC_UNUSED Test *test, SPICE_GNUC_UNUSED Command *command)
{
    if (g_surface_data) {
        g_free(g_surface_data);
        g_surface_data = NULL;
    }
}

static Command commands[] = {
    {SIMPLE_CREATE_SURFACE, set_surface_params, .cb_opaque = NULL},
    {SIMPLE_DRAW_SOLID, set_draw_parameters, .cb_opaque = NULL},
    {SIMPLE_DRAW_SOLID, set_draw_parameters, .cb_opaque = NULL},
    {SIMPLE_DRAW_SOLID, set_draw_parameters, .cb_opaque = NULL},
    {SIMPLE_DRAW_SOLID, set_draw_parameters, .cb_opaque = NULL},
    {SIMPLE_DRAW_SOLID, set_draw_parameters, .cb_opaque = NULL},
    {SIMPLE_DRAW_SOLID, set_draw_parameters, .cb_opaque = NULL},
    {SIMPLE_DRAW_SOLID, set_draw_parameters, .cb_opaque = NULL},
    {SIMPLE_DRAW_SOLID, set_draw_parameters, .cb_opaque = NULL},
    {SIMPLE_DRAW_SOLID, set_draw_parameters, .cb_opaque = NULL},
    {SIMPLE_DESTROY_SURFACE, set_destroy_parameters, .cb_opaque = NULL},
};

static void on_client_connected(Test *test)
{
    test_set_command_list(test, commands, G_N_ELEMENTS(commands));
}

int main(void)
{
    SpiceCoreInterface *core;
    Test *test;

    core = basic_event_loop_init();
    test = test_new(core);
    test->on_client_connected = on_client_connected;
    //spice_server_set_image_compression(server, SPICE_IMAGE_COMPRESSION_OFF);
    test_add_display_interface(test);

    ping_timer = core->timer_add(pinger, test);
    core->timer_start(ping_timer, ping_ms);

    basic_event_loop_mainloop();
    test_destroy(test);

    return 0;
}
