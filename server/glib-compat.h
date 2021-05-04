/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2021 Red Hat, Inc.

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

#ifndef GLIB_COMPAT_H_
#define GLIB_COMPAT_H_

#include <glib.h>

#if GLIB_VERSION_MIN_REQUIRED >= G_ENCODE_VERSION(2, 68)
#error Time to remove this section
#elif !GLIB_CHECK_VERSION(2,68,0)
static inline void*
g_memdup2(const void *ptr, size_t size)
{
    void *dst = NULL;

    if (ptr && size != 0) {
        dst = g_malloc(size);
        memcpy(dst, ptr, size);
    }
    return dst;
}
#elif GLIB_VERSION_MAX_ALLOWED < G_ENCODE_VERSION(2, 68)
static inline void*
g_memdup2_compat(const void *ptr, size_t size)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    return g_memdup2(ptr, size);
    G_GNUC_END_IGNORE_DEPRECATIONS
}
#define g_memdup2 g_memdup2_compat
#endif

#endif /* GLIB_COMPAT_H_ */
