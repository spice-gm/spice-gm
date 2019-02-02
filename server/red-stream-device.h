/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2018 Red Hat, Inc.

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

#ifndef STREAM_DEVICE_H
#define STREAM_DEVICE_H

#include "display-limits.h"
#include "char-device.h"

G_BEGIN_DECLS

/**
 * StreamDevice inherits from RedCharDevice.
 */
typedef struct StreamDevice StreamDevice;
typedef struct StreamDeviceClass StreamDeviceClass;

#define TYPE_STREAM_DEVICE stream_device_get_type()

#define STREAM_DEVICE(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_STREAM_DEVICE, StreamDevice))
#define STREAM_DEVICE_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), TYPE_STREAM_DEVICE, StreamDeviceClass))
#define IS_STREAM_DEVICE(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), TYPE_STREAM_DEVICE))
#define STREAM_DEVICE_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), TYPE_STREAM_DEVICE, StreamDeviceClass))

GType stream_device_get_type(void) G_GNUC_CONST;

typedef struct StreamDeviceDisplayInfo {
    uint32_t stream_id;
    char device_address[MAX_DEVICE_ADDRESS_LEN];
    uint32_t device_display_id;
} StreamDeviceDisplayInfo;

StreamDevice *stream_device_connect(RedsState *reds, SpiceCharDeviceInstance *sin);

/* Create channel for the streaming device.
 * If the channel already exists the function does nothing.
 */
void stream_device_create_channel(StreamDevice *dev);

const StreamDeviceDisplayInfo *stream_device_get_device_display_info(StreamDevice *dev);

/**
 * Returns -1 if the StreamDevice doesn't have a channel yet.
 */
int32_t stream_device_get_stream_channel_id(StreamDevice *dev);

G_END_DECLS

#endif /* STREAM_DEVICE_H */
