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

#ifndef SOUND_H_
#define SOUND_H_

#include "red-common.h"

#include "push-visibility.h"

struct RedClient;

void snd_attach_playback(RedsState *reds, SpicePlaybackInstance *sin);
void snd_detach_playback(SpicePlaybackInstance *sin);

void snd_attach_record(RedsState *reds, SpiceRecordInstance *sin);
void snd_detach_record(SpiceRecordInstance *sin);

void snd_set_playback_compression(bool on);

void snd_set_playback_latency(struct RedClient *client, uint32_t latency);

#include "pop-visibility.h"

#endif /* SOUND_H_ */
