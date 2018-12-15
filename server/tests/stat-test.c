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

/* check statistics
 */

#include <config.h>

#undef COMPRESS_STAT
#undef RED_WORKER_STAT

#if TEST_COMPRESS_STAT
#define COMPRESS_STAT
#endif

#if TEST_RED_WORKER_STAT
#define RED_WORKER_STAT
#endif

#include <unistd.h>
#include <glib.h>
#include <common/macros.h>

#include "../stat.h"

#ifndef TEST_NAME
#error TEST_NAME must be defined!
#endif

// avoid warning, the function is called by stat-main.c
void TEST_NAME(void);

void TEST_NAME(void)
{
    stat_info_t info;
    stat_start_time_t start_time;

#if !defined(COMPRESS_STAT) && !defined(RED_WORKER_STAT)
    SPICE_VERIFY(sizeof(start_time) == 0);
    SPICE_VERIFY(sizeof(info) == 0);
#endif

    stat_init(&info, "test", CLOCK_MONOTONIC);
    stat_start_time_init(&start_time, &info);
    usleep(2000);
    stat_add(&info, start_time);

#ifdef RED_WORKER_STAT
    g_assert_cmpuint(info.count, ==, 1);
    g_assert_cmpuint(info.min, ==, info.max);
    g_assert_cmpuint(info.min, >=, 2000000);
    g_assert_cmpuint(info.min, <, 100000000);
#endif

    stat_reset(&info);

    stat_compress_init(&info, "test", CLOCK_MONOTONIC);
    stat_start_time_init(&start_time, &info);
    usleep(2000);
    stat_compress_add(&info, start_time, 100, 50);
    usleep(1000);
    stat_compress_add(&info, start_time, 1000, 500);

#ifdef COMPRESS_STAT
    g_assert_cmpuint(info.count, ==, 2);
    g_assert_cmpuint(info.min, !=, info.max);
    g_assert_cmpuint(info.min, >=, 2000000);
    g_assert_cmpuint(info.min, <, 100000000);
    g_assert_cmpuint(info.total, >=, 5000000);
    g_assert_cmpuint(info.orig_size, ==, 1100);
    g_assert_cmpuint(info.comp_size, ==, 550);
#endif
}
