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
#include <stdio.h>
#include <sys/time.h>
#include <math.h>

#include <spice.h>
#include "basic-event-loop.h"

/* test the audio playback interface. Really basic no frils test - create
 * a single tone sinus sound (easy to hear clicks if it is generated badly
 * or is transmitted badly).
 *
 * TODO: Was going to do white noise to test compression too.
 *
 * TODO: gstreamer based test (could be used to play music files so
 * it has actual merit. Also possibly to simulate network effects?)
 * */

static SpicePlaybackInstance playback_instance;

static const SpicePlaybackInterface playback_sif = {
    .base = {
        .type          = SPICE_INTERFACE_PLAYBACK,
        .description   = "test playback",
        .major_version = SPICE_INTERFACE_PLAYBACK_MAJOR,
        .minor_version = SPICE_INTERFACE_PLAYBACK_MINOR,
    }
};

static uint32_t *frame;
static uint32_t num_samples;
static SpiceTimer *playback_timer;
static int playback_timer_ms;
static SpiceCoreInterface *core;

static void get_frame(void)
{
    if (frame) {
        return;
    }
    spice_server_playback_get_buffer(&playback_instance, &frame, &num_samples);
    playback_timer_ms = num_samples
                        ? 1000 * num_samples / SPICE_INTERFACE_PLAYBACK_FREQ
                        : 100;
}

static void playback_timer_cb(SPICE_GNUC_UNUSED void *opaque)
{
    static int t = 0;
    static uint64_t last_sent_usec = 0;
    static uint64_t samples_to_send;
    uint32_t i;
    struct timeval cur;
    uint64_t cur_usec;

    get_frame();
    if (!frame) {
        /* continue waiting until there is a client */
        core->timer_start(playback_timer, 100);
        return;
    }

    /* we have a client */
    gettimeofday(&cur, NULL);
    cur_usec = cur.tv_usec + cur.tv_sec * 1e6;
    if (last_sent_usec == 0) {
        samples_to_send = num_samples;
    } else {
        samples_to_send += (cur_usec - last_sent_usec) * SPICE_INTERFACE_PLAYBACK_FREQ / 1e6;
    }
    last_sent_usec = cur_usec;
    while (samples_to_send > num_samples && frame) {
        samples_to_send -= num_samples;
        for (i = 0 ; i < num_samples; ++i) {
            uint16_t level = (1<<14) * sin((t+i)/10.0);
            frame[i] = (level << 16) + level;
        }
        t += num_samples;
        spice_server_playback_put_samples(&playback_instance, frame);
        frame = NULL;
        get_frame();
    }
    core->timer_start(playback_timer, playback_timer_ms);
}

int main(void)
{
    SpiceServer *server = spice_server_new();
    core = basic_event_loop_init();

    spice_server_set_port(server, 5701);
    spice_server_set_noauth(server);
    spice_server_init(server, core);

    playback_instance.base.sif = &playback_sif.base;
    spice_server_add_interface(server, &playback_instance.base);
    spice_server_playback_start(&playback_instance);

    playback_timer_ms = 100;
    playback_timer = core->timer_add(playback_timer_cb, NULL);
    core->timer_start(playback_timer, playback_timer_ms);

    basic_event_loop_mainloop();

    return 0;
}
