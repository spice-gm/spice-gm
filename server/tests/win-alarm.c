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

#include <config.h>
#include <glib.h>

#ifdef _WIN32
#include <windows.h>
#include "win-alarm.h"

static HANDLE alarm_cond = NULL;

static DWORD WINAPI alarm_thread_proc(LPVOID arg)
{
    unsigned int timeout = (uintptr_t) arg;
    switch (WaitForSingleObject(alarm_cond, timeout * 1000)) {
    case WAIT_OBJECT_0:
        return 0;
    }
    abort();
    return 0;
}

void alarm(unsigned int timeout)
{
    static HANDLE thread = NULL;

    // create an event to stop the alarm thread
    if (alarm_cond == NULL) {
        alarm_cond = CreateEvent(NULL, TRUE, FALSE, NULL);
        g_assert(alarm_cond != NULL);
    }

    // stop old alarm
    if (thread) {
        SetEvent(alarm_cond);
        g_assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
        CloseHandle(thread);
        thread = NULL;
    }

    if (timeout) {
        ResetEvent(alarm_cond);

        // start alarm thread
        thread = CreateThread(NULL, 0, alarm_thread_proc, (LPVOID) (uintptr_t) timeout, 0, NULL);
        g_assert(thread);
    }
}
#endif
