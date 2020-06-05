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
#include <config.h>

#include "red-channel.h"
#include "red-pipe-item.h"

RedPipeItem::RedPipeItem(int init_type):
    type(init_type)
{
}

RedPipeItem *red_pipe_item_ref(RedPipeItem *item)
{
    // this call should be replaced by shared_ptr instead
    shared_ptr_add_ref(item);

    return item;
}

void red_pipe_item_unref(RedPipeItem *item)
{
    // this call should be replaced by shared_ptr instead
    shared_ptr_unref(item);
}

static void marshaller_unref_pipe_item(uint8_t *, void *opaque)
{
    RedPipeItem *item = (RedPipeItem*) opaque;
    red_pipe_item_unref(item);
}

void RedPipeItem::add_to_marshaller(SpiceMarshaller *m, uint8_t *data, size_t size)
{
    red_pipe_item_ref(this);
    spice_marshaller_add_by_ref_full(m, data, size,
                                     marshaller_unref_pipe_item, this);
}
