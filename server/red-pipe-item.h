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

#ifndef RED_PIPE_ITEM_H_
#define RED_PIPE_ITEM_H_

#include <stddef.h>
#include <inttypes.h>

typedef struct RedPipeItem RedPipeItem;

typedef void red_pipe_item_free_t(RedPipeItem *item);

struct RedPipeItem {
    int type;

    /* private */
    int refcount;

    red_pipe_item_free_t *free_func;
};

void red_pipe_item_init_full(RedPipeItem *item, int type, red_pipe_item_free_t free_func);
RedPipeItem *red_pipe_item_ref(RedPipeItem *item);
void red_pipe_item_unref(RedPipeItem *item);

static inline void red_pipe_item_init(RedPipeItem *item, int type)
{
    red_pipe_item_init_full(item, type, NULL);
}

/* a convenience function for unreffing a pipe item after it has been sent */
void marshaller_unref_pipe_item(uint8_t *data, void *opaque);

#endif /* RED_PIPE_ITEM_H_ */
