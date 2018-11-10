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
/*
 * Test some red_record_ APIs.
 */
#include <config.h>

#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#include "test-glib-compat.h"
#include "red-record-qxl.h"

#define OUTPUT_FILENAME "rec1.txt"

static void
test_record(bool compress)
{
    RedRecord *rec;
    const char *fn = OUTPUT_FILENAME;

    unsetenv("SPICE_WORKER_RECORD_FILTER");
    if (compress) {
        setenv("SPICE_WORKER_RECORD_FILTER", "gzip", 1);
    }

    // delete possible stale test output
    unlink(fn);
    g_assert_cmpint(access(fn, F_OK), <, 0);

    // create recorder
    rec = red_record_new(fn);
    g_assert_nonnull(rec);

    // check file was created by recorder
    g_assert_cmpint(access(fn, F_OK), ==, 0);

    g_assert_nonnull(red_record_ref(rec));
    red_record_unref(rec);

    // record something
    red_record_event(rec, 1, 123);

    red_record_unref(rec);

    // check content of the output file
    FILE *f;
    if (!compress) {
        f = fopen(fn, "r");
    } else {
        f = popen("gzip -dc < " OUTPUT_FILENAME, "r");
    }
    g_assert_nonnull(f);

    char line[1024];
    int version;
    g_assert_nonnull(fgets(line, sizeof(line), f));
    g_assert_cmpint(sscanf(line, "SPICE_REPLAY %d", &version), ==, 1);

    int w, t;
    g_assert_nonnull(fgets(line, sizeof(line), f));
    g_assert_cmpint(sscanf(line, "event %*d %d %d", &w, &t), ==, 2);
    g_assert_cmpint(w, ==, 1);
    g_assert_cmpint(t, ==, 123);

    g_assert_null(fgets(line, sizeof(line), f));

    if (!compress) {
        fclose(f);
    } else {
        pclose(f);
    }

    // clean test output file
    unlink(fn);
}

int
main(void)
{
    test_record(false);
    test_record(true);
    return 0;
}
