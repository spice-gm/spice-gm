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

#ifndef STAT_H_
#define STAT_H_

#include <stdint.h>
#include <glib.h>

#include "spice-wrapped.h"
#include "stat-file.h"

SPICE_BEGIN_DECLS

typedef struct {
#ifdef RED_STATISTICS
    uint64_t *counter;
#endif
    uint8_t dummy_empty_field[0]; /* C/C++ compatibility */
} RedStatCounter;

typedef struct {
#ifdef RED_STATISTICS
    uint32_t ref;
#endif
    uint8_t dummy_empty_field[0]; /* C/C++ compatibility */
} RedStatNode;

#ifdef RED_STATISTICS
void stat_init_node(RedStatNode *node, SpiceServer *reds,
                    const RedStatNode *parent, const char *name, int visible);
void stat_remove_node(SpiceServer *reds, RedStatNode *node);
void stat_init_counter(RedStatCounter *counter, SpiceServer *reds,
                       const RedStatNode *parent, const char *name, int visible);
void stat_remove_counter(SpiceServer *reds, RedStatCounter *counter);

#else

static inline void
stat_init_node(RedStatNode *node, SpiceServer *reds,
               const RedStatNode *parent, const char *name, int visible)
{
}

static inline void
stat_remove_node(SpiceServer *reds, RedStatNode *node)
{
}

static inline void
stat_init_counter(RedStatCounter *counter, SpiceServer *reds,
                  const RedStatNode *parent, const char *name, int visible)
{
}

static inline void
stat_remove_counter(SpiceServer *reds, RedStatCounter *counter)
{
}
#endif /* RED_STATISTICS */

static inline void
stat_inc_counter(RedStatCounter counter, uint64_t value)
{
#ifdef RED_STATISTICS
    if (counter.counter) {
        *(counter.counter) += value;
    }
#endif
}

typedef uint64_t stat_time_t;

static inline stat_time_t stat_now(clockid_t clock_id)
{
    struct timespec ts;

    clock_gettime(clock_id, &ts);
    return ts.tv_nsec + (uint64_t) ts.tv_sec * (1000 * 1000 * 1000);
}

typedef struct {
#if defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
    stat_time_t time;
#endif
    uint8_t dummy_empty_field[0]; /* C/C++ compatibility */
} stat_start_time_t;

#if defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
static inline double stat_cpu_time_to_sec(stat_time_t time)
{
    return (double)time / (1000 * 1000 * 1000);
}
#endif

typedef struct {
#if defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
    const char *name;
    clockid_t clock;
    uint32_t count;
    stat_time_t max;
    stat_time_t min;
    stat_time_t total;
#ifdef COMPRESS_STAT
    uint64_t orig_size;
    uint64_t comp_size;
#endif
#endif
    uint8_t dummy_empty_field[0]; /* C/C++ compatibility */
} stat_info_t;

static inline void stat_start_time_init(G_GNUC_UNUSED stat_start_time_t *tm,
                                        G_GNUC_UNUSED const stat_info_t *info)
{
#if defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
    tm->time = stat_now(info->clock);
#endif
}

static inline void stat_reset(G_GNUC_UNUSED stat_info_t *info)
{
#if defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
    info->count = info->max = info->total = 0;
    info->min = ~(stat_time_t)0;
#ifdef COMPRESS_STAT
    info->orig_size = info->comp_size = 0;
#endif
#endif
}

static inline void stat_init(G_GNUC_UNUSED stat_info_t *info,
                             G_GNUC_UNUSED const char *name,
                             G_GNUC_UNUSED clockid_t clock)
{
#if defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
    info->name = name;
    info->clock = clock;
    stat_reset(info);
#endif
}

static inline void stat_compress_init(G_GNUC_UNUSED stat_info_t *info,
                                      G_GNUC_UNUSED const char *name,
                                      G_GNUC_UNUSED clockid_t clock)
{
    stat_init(info, name, clock);
}

static inline void stat_compress_add(G_GNUC_UNUSED stat_info_t *info,
                                     G_GNUC_UNUSED stat_start_time_t start,
                                     G_GNUC_UNUSED int orig_size,
                                     G_GNUC_UNUSED int comp_size)
{
#ifdef COMPRESS_STAT
    stat_time_t time;
    ++info->count;
    time = stat_now(info->clock) - start.time;
    info->total += time;
    info->max = MAX(info->max, time);
    info->min = MIN(info->min, time);
    info->orig_size += orig_size;
    info->comp_size += comp_size;
#endif
}

static inline double stat_byte_to_mega(uint64_t size)
{
    return (double)size / (1000 * 1000);
}

static inline void stat_add(G_GNUC_UNUSED stat_info_t *info,
                            G_GNUC_UNUSED stat_start_time_t start)
{
#ifdef RED_WORKER_STAT
    stat_time_t time;
    ++info->count;
    time = stat_now(info->clock) - start.time;
    info->total += time;
    info->max = MAX(info->max, time);
    info->min = MIN(info->min, time);
#endif
}

SPICE_END_DECLS

#endif /* STAT_H_ */
