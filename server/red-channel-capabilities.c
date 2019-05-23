/*
   Copyright (C) 2017 Red Hat, Inc.

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

#include <string.h>
#include <glib.h>
#include <common/macros.h>

#include "red-channel-capabilities.h"

void red_channel_capabilities_init(RedChannelCapabilities *dest,
                                   const RedChannelCapabilities *caps)
{
    *dest = *caps;
    if (caps->common_caps) {
        dest->common_caps = (uint32_t*) g_memdup(caps->common_caps,
                                     caps->num_common_caps * sizeof(uint32_t));
    }
    if (caps->num_caps) {
        dest->caps = (uint32_t*) g_memdup(caps->caps, caps->num_caps * sizeof(uint32_t));
    }
}

void red_channel_capabilities_reset(RedChannelCapabilities *caps)
{
    g_free(caps->common_caps);
    g_free(caps->caps);
    memset(caps, 0, sizeof(*caps));
}
