/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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

#ifndef CURSOR_CHANNEL_H_
#define CURSOR_CHANNEL_H_

#include "common-graphics-channel.h"
#include "red-parse-qxl.h"
#include "dispatcher.h"

#include "push-visibility.h"

struct RedCursorPipeItem;

/**
 * This type it's a RedChannel class which implement cursor (mouse)
 * movements.
 */
struct CursorChannel final: public CommonGraphicsChannel
{
    CursorChannel(RedsState *reds, uint32_t id,
                  SpiceCoreInterfaceInternal *core=nullptr, Dispatcher *dispatcher=nullptr);
    ~CursorChannel();
    void reset();
    void do_init();
    void process_cmd(RedCursorCmd *cursor_cmd);
    void set_mouse_mode(uint32_t mode);
    void on_connect(RedClient *client, RedStream *stream, int migration,
                    RedChannelCapabilities *caps) override;

    red::shared_ptr<RedCursorPipeItem> item;
    bool cursor_visible = true;
    SpicePoint16 cursor_position;
    uint16_t cursor_trail_length;
    uint16_t cursor_trail_frequency;
    uint32_t mouse_mode = SPICE_MOUSE_MODE_SERVER;
};


/**
 * Create CursorChannel.
 * Since CursorChannel is intended to be run in a separate thread,
 * the function accepts a dispatcher parameter to allows some
 * operations to be executed in the channel thread.
 */
red::shared_ptr<CursorChannel> cursor_channel_new(RedsState *server, int id,
                                                  SpiceCoreInterfaceInternal *core,
                                                  Dispatcher *dispatcher);

#include "pop-visibility.h"

#endif /* CURSOR_CHANNEL_H_ */
