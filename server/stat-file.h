/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2016 Red Hat, Inc.

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

#ifndef STAT_FILE_H_
#define STAT_FILE_H_

#include <stdint.h>

SPICE_BEGIN_DECLS

typedef uint32_t StatNodeRef;
#define INVALID_STAT_REF (~(StatNodeRef)0)

typedef struct RedStatFile RedStatFile;

RedStatFile *stat_file_new(unsigned int max_nodes);
void stat_file_free(RedStatFile *stat_file);
void stat_file_unlink(RedStatFile *file_stat);
const char *stat_file_get_shm_name(RedStatFile *stat_file);
StatNodeRef stat_file_add_node(RedStatFile *stat_file, StatNodeRef parent,
                               const char *name, int visible);
uint64_t *stat_file_add_counter(RedStatFile *stat_file, StatNodeRef parent,
                                const char *name, int visible);
void stat_file_remove_node(RedStatFile *stat_file, StatNodeRef ref);
void stat_file_remove_counter(RedStatFile *stat_file, uint64_t *counter);

SPICE_END_DECLS

#endif /* STAT_FILE_H_ */
