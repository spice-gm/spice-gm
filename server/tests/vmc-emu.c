/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2019 Red Hat, Inc.

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
#include <glib.h>

#include "vmc-emu.h"

// handle writes to the device
static int vmc_write(SpiceCharDeviceInstance *sin,
                     const uint8_t *buf, int len)
{
    VmcEmu *const vmc = SPICE_CONTAINEROF(sin, VmcEmu, instance);

    // just copy into the buffer
    unsigned copy = MIN(sizeof(vmc->write_buf) - vmc->write_pos, len);
    memcpy(vmc->write_buf+vmc->write_pos, buf, copy);
    vmc->write_pos += copy;
    if (copy && vmc->data_written_cb) {
        vmc->data_written_cb(vmc);
    }
    return len;
}

static int vmc_read(SpiceCharDeviceInstance *sin,
                    uint8_t *buf, int len)
{
    VmcEmu *const vmc = SPICE_CONTAINEROF(sin, VmcEmu, instance);
    int ret;

    if (vmc->pos >= *vmc->message_sizes_curr && vmc->message_sizes_curr < vmc->message_sizes_end) {
        ++vmc->message_sizes_curr;
    }
    if (vmc->message_sizes_curr >= vmc->message_sizes_end || vmc->pos >= *vmc->message_sizes_curr) {
        return 0;
    }
    ret = MIN(*vmc->message_sizes_curr - vmc->pos, len);
    memcpy(buf, &vmc->message[vmc->pos], ret);
    vmc->pos += ret;
    // kick off next message read
    // currently Qemu kicks the device so we need to do it manually
    // here. If not all data are read, the device goes into blocking
    // state and we get the wake only when we read from the device
    // again
    if (vmc->pos >= *vmc->message_sizes_curr) {
        spice_server_char_device_wakeup(&vmc->instance);
    }
    return ret;
}

static void vmc_state(SpiceCharDeviceInstance *sin,
                      int connected)
{
    VmcEmu *const vmc = SPICE_CONTAINEROF(sin, VmcEmu, instance);
    vmc->device_enabled = !!connected;
}

static const SpiceCharDeviceInterface vmc_interface = {
    .base = {
        .type          = SPICE_INTERFACE_CHAR_DEVICE,
        .description   = "test spice virtual channel char device",
        .major_version = SPICE_INTERFACE_CHAR_DEVICE_MAJOR,
        .minor_version = SPICE_INTERFACE_CHAR_DEVICE_MINOR,
    },
    .state              = vmc_state,
    .write              = vmc_write,
    .read               = vmc_read,
};

VmcEmu *vmc_emu_new(const char *subtype, const char *portname)
{
    VmcEmu *vmc = g_new0(VmcEmu, 1);
    vmc->vmc_interface = vmc_interface;
    vmc->instance.base.sif = &vmc->vmc_interface.base;
    vmc->instance.subtype = g_strdup(subtype);
    if (portname) {
        vmc->instance.portname = g_strdup(portname);
    }
    vmc_emu_reset(vmc);
    return vmc;
}

void vmc_emu_destroy(VmcEmu *vmc)
{
    g_free((char *) vmc->instance.portname);
    g_free((char *) vmc->instance.subtype);
    g_free(vmc);
}

void vmc_emu_reset(VmcEmu *vmc)
{
    vmc->pos = 0;
    vmc->write_pos = 0;
    vmc->message_sizes_curr = vmc->message_sizes;
    vmc->message_sizes_end = vmc->message_sizes;
}

void vmc_emu_add_read_till(VmcEmu *vmc, uint8_t *end)
{
    g_assert(vmc->message_sizes_end - vmc->message_sizes < G_N_ELEMENTS(vmc->message_sizes));
    g_assert(end >= vmc->message);
    g_assert(end - vmc->message <= G_N_ELEMENTS(vmc->message));
    unsigned prev_size =
        vmc->message_sizes_end > vmc->message_sizes ? vmc->message_sizes_end[-1] : 0;
    unsigned size = end - vmc->message;
    g_assert(size >= prev_size);
    *vmc->message_sizes_end = size;
    ++vmc->message_sizes_end;
}
