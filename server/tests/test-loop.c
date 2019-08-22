/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2015 Red Hat, Inc.

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

/* Test event loop
 */

#include <config.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <glib.h>

#include <spice/macros.h>
#include <common/log.h>
#include "basic-event-loop.h"
#include "win-alarm.h"

static SpiceCoreInterface *core = NULL;
static GMainLoop *loop = NULL;
static pthread_t loop_thread;

static void timer_err(void *opaque)
{
    spice_assert(0);
}

static SpiceTimer *to_delete_timer = NULL;
static void timer_del(void *opaque)
{
    spice_assert(core);
    spice_assert(to_delete_timer);

    spice_assert(pthread_equal(loop_thread, pthread_self()));

    core->timer_remove(to_delete_timer);
    to_delete_timer = NULL;
}

static void timer_exit(void *opaque)
{
    spice_assert(loop);

    spice_assert(pthread_equal(loop_thread, pthread_self()));

    g_main_loop_quit(loop);
}

static void *loop_func(void *arg)
{
    loop_thread = pthread_self();

    spice_assert(loop);

    g_main_loop_run(loop);

    return NULL;
}

static SpiceTimer *twice_timers_remove[2] = { NULL, NULL };
static int twice_remove_called = 0;
static void timer_not_twice_remove(void *opaque)
{
    ++twice_remove_called;
    spice_assert(twice_remove_called == 1);

    /* delete timers, should not have another call */
    core->timer_remove(twice_timers_remove[0]);
    core->timer_remove(twice_timers_remove[1]);
    twice_timers_remove[0] = NULL;
    twice_timers_remove[1] = NULL;
}

static SpiceTimer *twice_timers_cancel[2] = { NULL, NULL };
static int twice_cancel_called = 0;
static void timer_not_twice(void *opaque)
{
    spice_assert(++twice_cancel_called == 1);

    /* cancel timers, should not have another call */
    core->timer_cancel(twice_timers_cancel[0]);
    core->timer_cancel(twice_timers_cancel[1]);
}


int main(int argc, char **argv)
{
    SpiceTimer *timer, *timers[10];
    int i, rc;

    memset(timers, 0, sizeof(timers));

    core = basic_event_loop_init();

    i = 0;

    /* add a timer and delete to check is correctly deleted */
    timer = core->timer_add(timer_err, NULL);
    core->timer_start(timer, 1);
    core->timer_remove(timer);

    /* create timer, should not be executed */
    timer = timers[i++] = core->timer_add(timer_err, NULL);

    /* add a timer and cancel to check is not executed */
    timer = timers[i++] = core->timer_add(timer_err, NULL);
    core->timer_start(timer, 1);
    core->timer_cancel(timer);

    /* check we can remove timer inside a timer */
    timer = to_delete_timer = core->timer_add(timer_del, NULL);
    spice_assert(to_delete_timer != NULL);
    core->timer_start(timer, 1);

    /* create a timer that does something */
    timer = timers[i++] = core->timer_add(timer_exit, NULL);
    core->timer_start(timer, 10);

    /* test events are not called when freed */
    timer = twice_timers_remove[0] = core->timer_add(timer_not_twice_remove, NULL);
    spice_assert(timer != NULL);
    core->timer_start(timer, 2);
    timer = twice_timers_remove[1] = core->timer_add(timer_not_twice_remove, NULL);
    spice_assert(timer != NULL);
    core->timer_start(timer, 2);

    /* test events are not called when cancelled */
    timer = timers[i++] = twice_timers_cancel[0] = core->timer_add(timer_not_twice, core);
    spice_assert(timer != NULL);
    core->timer_start(timer, 4);
    timer = timers[i++] = twice_timers_cancel[1] = core->timer_add(timer_not_twice, core);
    spice_assert(timer != NULL);
    core->timer_start(timer, 4);

    /* run the loop */
    loop = g_main_loop_new(basic_event_loop_get_context(), FALSE);
    alarm(1);
    rc = pthread_create(&loop_thread, NULL, loop_func, NULL);
    spice_assert(rc == 0);
    rc = pthread_join(loop_thread, NULL);
    spice_assert(rc == 0);
    alarm(0);
    g_main_loop_unref(loop);

    /* delete executed ? */
    spice_assert(to_delete_timer == NULL);

    /* cleanup */
    for (i = 0; i < G_N_ELEMENTS(timers); ++i) {
        if (timers[i]) {
            core->timer_remove(timers[i]);
            timers[i] = NULL;
        }
    }

    basic_event_loop_destroy();

    return 0;
}
