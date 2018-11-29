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

/* Do some tests on memory parsing
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#undef NDEBUG
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <spice/macros.h>
#include "memslot.h"
#include "red-parse-qxl.h"
#include "test-glib-compat.h"

static QXLPHYSICAL
to_physical(const void *ptr)
{
    return (uintptr_t) ptr;
}

static void *
from_physical(QXLPHYSICAL physical)
{
    return (void *)(uintptr_t) physical;
}

static void*
create_chunk(size_t prefix, uint32_t size, QXLDataChunk* prev, int fill)
{
    uint8_t *ptr = g_malloc0(prefix + sizeof(QXLDataChunk) + size);
    QXLDataChunk *qxl = (QXLDataChunk *) (ptr + prefix);
    memset(&qxl->data[0], fill, size);
    qxl->data_size = size;
    qxl->prev_chunk = to_physical(prev);
    if (prev)
        prev->next_chunk = to_physical(qxl);
    return ptr;
}

static void init_meminfo(RedMemSlotInfo *mem_info)
{
    memslot_info_init(mem_info, 1 /* groups */, 1 /* slots */, 1, 1, 0);
    memslot_info_add_slot(mem_info, 0, 0, 0 /* delta */, 0 /* start */, ~0ul /* end */, 0 /* generation */);
}

static void init_qxl_surface(QXLSurfaceCmd *qxl)
{
    void *surface_mem;

    memset(qxl, 0, sizeof(*qxl));

    qxl->surface_id = 123;

    qxl->u.surface_create.format = SPICE_SURFACE_FMT_32_xRGB;
    qxl->u.surface_create.width = 128;
    qxl->u.surface_create.stride = 512;
    qxl->u.surface_create.height = 128;
    surface_mem = g_malloc(0x10000);
    qxl->u.surface_create.data = to_physical(surface_mem);
}

static void deinit_qxl_surface(QXLSurfaceCmd *qxl)
{
    g_free(from_physical(qxl->u.surface_create.data));
}

static void test_memslot_invalid_group_id(void)
{
    RedMemSlotInfo mem_info;
    init_meminfo(&mem_info);

    memslot_get_virt(&mem_info, 0, 16, 1);
}

static void test_memslot_invalid_slot_id(void)
{
    RedMemSlotInfo mem_info;
    init_meminfo(&mem_info);

    memslot_get_virt(&mem_info, 1 << mem_info.memslot_id_shift, 16, 0);
}

static void test_memslot_invalid_addresses(void)
{
    g_test_trap_subprocess("/server/memslot-invalid-addresses/subprocess/group_id", 0, 0);
    g_test_trap_assert_stderr("*group_id too big*");

    g_test_trap_subprocess("/server/memslot-invalid-addresses/subprocess/slot_id", 0, 0);
    g_test_trap_assert_stderr("*slot_id 1 too big*");
}

static void test_no_issues(void)
{
    RedMemSlotInfo mem_info;
    RedSurfaceCmd *cmd;
    QXLSurfaceCmd qxl;

    init_meminfo(&mem_info);
    init_qxl_surface(&qxl);

    /* try to create a surface with no issues, should succeed */
    cmd = red_surface_cmd_new(NULL, &mem_info, 0, to_physical(&qxl));
    g_assert_nonnull(cmd);
    red_surface_cmd_unref(cmd);

    deinit_qxl_surface(&qxl);
    memslot_info_destroy(&mem_info);
}

static void test_stride_too_small(void)
{
    RedMemSlotInfo mem_info;
    RedSurfaceCmd *cmd;
    QXLSurfaceCmd qxl;

    init_meminfo(&mem_info);
    init_qxl_surface(&qxl);

    /* try to create a surface with a stride too small to fit
     * the entire width.
     * This can be used to cause buffer overflows so refuse it.
     */
    qxl.u.surface_create.stride = 256;
    cmd = red_surface_cmd_new(NULL, &mem_info, 0, to_physical(&qxl));
    g_assert_null(cmd);

    deinit_qxl_surface(&qxl);
    memslot_info_destroy(&mem_info);
}

static void test_too_big_image(void)
{
    RedMemSlotInfo mem_info;
    RedSurfaceCmd *cmd;
    QXLSurfaceCmd qxl;

    init_meminfo(&mem_info);
    init_qxl_surface(&qxl);

    /* try to create a surface quite large.
     * The sizes (width and height) were chosen so the multiplication
     * using 32 bit values gives a very small value.
     * These kind of values should be refused as they will cause
     * overflows. Also the total memory for the card is not enough to
     * hold the surface so surely can't be accepted.
     */
    qxl.u.surface_create.stride = 0x08000004 * 4;
    qxl.u.surface_create.width = 0x08000004;
    qxl.u.surface_create.height = 0x40000020;
    cmd = red_surface_cmd_new(NULL, &mem_info, 0, to_physical(&qxl));
    g_assert_null(cmd);

    deinit_qxl_surface(&qxl);
    memslot_info_destroy(&mem_info);
}

static void test_cursor_command(void)
{
    RedMemSlotInfo mem_info;
    RedCursorCmd *red_cursor_cmd;
    QXLCursorCmd cursor_cmd;
    QXLCursor *cursor;

    init_meminfo(&mem_info);

    /* test base cursor with no problems */
    memset(&cursor_cmd, 0, sizeof(cursor_cmd));
    cursor_cmd.type = QXL_CURSOR_SET;

    cursor = create_chunk(SPICE_OFFSETOF(QXLCursor, chunk), 128 * 128 * 4, NULL, 0xaa);
    cursor->header.unique = 1;
    cursor->header.width = 128;
    cursor->header.height = 128;
    cursor->data_size = 128 * 128 * 4;

    cursor_cmd.u.set.shape = to_physical(cursor);

    red_cursor_cmd = red_cursor_cmd_new(NULL, &mem_info, 0, to_physical(&cursor_cmd));
    g_assert_nonnull(red_cursor_cmd);
    red_cursor_cmd_unref(red_cursor_cmd);
    g_free(cursor);
    memslot_info_destroy(&mem_info);
}

static void test_circular_empty_chunks(void)
{
    RedMemSlotInfo mem_info;
    RedCursorCmd *red_cursor_cmd;
    QXLCursorCmd cursor_cmd;
    QXLCursor *cursor;
    QXLDataChunk *chunks[2];

    init_meminfo(&mem_info);
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*red_get_data_chunks_ptr: data split in too many chunks, avoiding DoS*");

    /* a circular list of empty chunks should not be a problems */
    memset(&cursor_cmd, 0, sizeof(cursor_cmd));
    cursor_cmd.type = QXL_CURSOR_SET;

    cursor = create_chunk(SPICE_OFFSETOF(QXLCursor, chunk), 0, NULL, 0xaa);
    cursor->header.unique = 1;
    cursor->header.width = 128;
    cursor->header.height = 128;
    cursor->data_size = 128 * 128 * 4;

    chunks[0] = create_chunk(0, 0, &cursor->chunk, 0xaa);
    chunks[0]->next_chunk = to_physical(&cursor->chunk);

    cursor_cmd.u.set.shape = to_physical(cursor);

    red_cursor_cmd = red_cursor_cmd_new(NULL, &mem_info, 0, to_physical(&cursor_cmd));
    if (red_cursor_cmd != NULL) {
        /* function does not return errors so there should be no data */
        g_assert_cmpuint(red_cursor_cmd->type, ==, QXL_CURSOR_SET);
        g_assert_cmpuint(red_cursor_cmd->u.set.position.x, ==, 0);
        g_assert_cmpuint(red_cursor_cmd->u.set.position.y, ==, 0);
        g_assert_cmpuint(red_cursor_cmd->u.set.shape.data_size, ==, 0);
        red_cursor_cmd_unref(red_cursor_cmd);
    }
    g_test_assert_expected_messages();

    g_free(cursor);
    g_free(chunks[0]);
    memslot_info_destroy(&mem_info);
}

static void test_circular_small_chunks(void)
{
    RedMemSlotInfo mem_info;
    RedCursorCmd *red_cursor_cmd;
    QXLCursorCmd cursor_cmd;
    QXLCursor *cursor;
    QXLDataChunk *chunks[2];

    init_meminfo(&mem_info);
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*red_get_data_chunks_ptr: data split in too many chunks, avoiding DoS*");

    /* a circular list of small chunks should not be a problems */
    memset(&cursor_cmd, 0, sizeof(cursor_cmd));
    cursor_cmd.type = QXL_CURSOR_SET;

    cursor = create_chunk(SPICE_OFFSETOF(QXLCursor, chunk), 1, NULL, 0xaa);
    cursor->header.unique = 1;
    cursor->header.width = 128;
    cursor->header.height = 128;
    cursor->data_size = 128 * 128 * 4;

    chunks[0] = create_chunk(0, 1, &cursor->chunk, 0xaa);
    chunks[0]->next_chunk = to_physical(&cursor->chunk);

    cursor_cmd.u.set.shape = to_physical(cursor);

    red_cursor_cmd = red_cursor_cmd_new(NULL, &mem_info, 0, to_physical(&cursor_cmd));
    if (red_cursor_cmd != NULL) {
        /* function does not return errors so there should be no data */
        g_assert_cmpuint(red_cursor_cmd->type, ==, QXL_CURSOR_SET);
        g_assert_cmpuint(red_cursor_cmd->u.set.position.x, ==, 0);
        g_assert_cmpuint(red_cursor_cmd->u.set.position.y, ==, 0);
        g_assert_cmpuint(red_cursor_cmd->u.set.shape.data_size, ==, 0);
        red_cursor_cmd_unref(red_cursor_cmd);
    }
    g_test_assert_expected_messages();

    g_free(cursor);
    g_free(chunks[0]);
    memslot_info_destroy(&mem_info);
}


int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* try to use invalid memslot group/slot */
    g_test_add_func("/server/memslot-invalid-addresses", test_memslot_invalid_addresses);
    g_test_add_func("/server/memslot-invalid-addresses/subprocess/group_id", test_memslot_invalid_group_id);
    g_test_add_func("/server/memslot-invalid-addresses/subprocess/slot_id", test_memslot_invalid_slot_id);

    /* try to create a surface with no issues, should succeed */
    g_test_add_func("/server/qxl-parsing-no-issues", test_no_issues);

    /* try to create a surface with a stride too small to fit
     * the entire width.
     * This can be used to cause buffer overflows so refuse it.
     */
    g_test_add_func("/server/qxl-parsing/stride-too-small", test_stride_too_small);

    /* try to create a surface quite large.
     * The sizes (width and height) were chosen so the multiplication
     * using 32 bit values gives a very small value.
     * These kind of values should be refused as they will cause
     * overflows. Also the total memory for the card is not enough to
     * hold the surface so surely can't be accepted.
     */
    g_test_add_func("/server/qxl-parsing/too-big-image", test_too_big_image);

    /* test base cursor with no problems */
    g_test_add_func("/server/qxl-parsing/base-cursor-command", test_cursor_command);

    /* a circular list of empty chunks should not be a problems */
    g_test_add_func("/server/qxl-parsing/circular-empty-chunks", test_circular_empty_chunks);

    /* a circular list of small chunks should not be a problems */
    g_test_add_func("/server/qxl-parsing/circular-small-chunks", test_circular_small_chunks);

    return g_test_run();
}
