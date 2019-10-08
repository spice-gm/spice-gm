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
#pragma once

#include "char-device.h"

typedef struct VmcEmu VmcEmu;

struct VmcEmu {
    SpiceCharDeviceInterface vmc_interface;
    SpiceCharDeviceInstance instance;

    // device buffer to read from
    uint8_t message[2048];
    // position to read from
    unsigned pos;

    // array of limits when the read should return
    // the array is defined as [message_sizes_curr, message_sizes_end)
    // then the size is reach we move on next one till exausted
    unsigned message_sizes[16];
    unsigned *message_sizes_end, *message_sizes_curr;

    bool device_enabled;

    unsigned write_pos;
    uint8_t write_buf[2048];

    // this callback will be called when new data arrive to the device
    void (*data_written_cb)(VmcEmu *vmc);
};

VmcEmu *vmc_emu_new(const char *subtype, const char *portname);
void vmc_emu_destroy(VmcEmu *vmc);
void vmc_emu_reset(VmcEmu *vmc);
void vmc_emu_add_read_till(VmcEmu *vmc, uint8_t *end);
