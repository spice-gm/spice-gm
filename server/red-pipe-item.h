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

/**
 * @file red-pipe-item.h
 * Generic declaration for objects contained in RedChannelClient pipe.
 */
#ifndef RED_PIPE_ITEM_H_
#define RED_PIPE_ITEM_H_

#include "red-common.h"
#include "utils.hpp"

#include "push-visibility.h"

/**
 * Base class for objects contained in RedChannelClient pipe
 */
struct RedPipeItem: public red::shared_ptr_counted
{
    SPICE_CXX_GLIB_ALLOCATOR
    void *operator new(size_t len, void *p)
    {
        return p;
    }

    /**
     * Allows to allocate a pipe item with additional space at the end.
     *
     * Used with structures like
     * @code{.cpp}
     * struct NameItem: public RedPipeItem {
     *   ...
     *   char name[];
     * }
     * ...
     * auto name_item = red::shared_ptr<NameItem>(new (6) NameItem(...));
     * strcpy(name_item->name, "hello");
     * @endcode
     */
    void *operator new(size_t size, size_t additional)
    {
        return g_malloc(size + additional);
    }

    RedPipeItem(int type);
    const int type;

    void add_to_marshaller(SpiceMarshaller *m, uint8_t *data, size_t size);
};

typedef red::shared_ptr<RedPipeItem> RedPipeItemPtr;

/* Most of the time the type is constant and we just add fields,
 * make it easier to initialize just with declaration
 */
template <int initial_type>
struct RedPipeItemNum: public RedPipeItem
{
    RedPipeItemNum():
        RedPipeItem(initial_type)
    {
    }
};

#include "pop-visibility.h"

#endif /* RED_PIPE_ITEM_H_ */
