/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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
/*
 * This test allocate and free some resources in order to be able to detect
 * leaks using a leak detector.
 *
 * To use with GCC/Clang address sanitizer you can set or add these options:
 *   CFLAGS="-fsanitize=address -fno-omit-frame-pointer"
 *   LDFLAGS="-fsanitize=address -lasan"
 * Note that early GCC address sanitizer don't have a leak detector.
 *
 * To use Valgrind you can run the test with:
 *   valgrind --tool=memcheck --leak-check=full ./test-leaks
 * For cleaner output you should suppress GLib checks with glib.supp file.
 */
#include <config.h>
#include <unistd.h>
#include <spice.h>

#include "test-glib-compat.h"
#include "basic-event-loop.h"
#include "test-display-base.h"

#define PKI_DIR SPICE_TOP_SRCDIR "/server/tests/pki/"

static void server_leaks(void)
{
    int result;
    SpiceCoreInterface *core;
    SpiceServer *server = spice_server_new();
    int sv[2];

    g_assert_nonnull(server);

    core = basic_event_loop_init();
    g_assert_nonnull(core);

    result = spice_server_set_tls(server, 5922,
                                  PKI_DIR "ca-cert.pem",
                                  PKI_DIR "server-cert.pem",
                                  PKI_DIR "server-key.pem",
                                  NULL, NULL, NULL);
    g_assert_cmpint(result, ==, 0);

    g_assert_cmpint(spice_server_init(server, core), ==, 0);

    /* cause the allocation of spice name */
    spice_server_set_name(server, "Test Spice Name");

    /* cause the allocation of security options */
    result = spice_server_set_channel_security(server, "main", SPICE_CHANNEL_SECURITY_SSL);
    g_assert_cmpint(result, ==, 0);

    /* spice_server_add_ssl_client should not leak when it's given a disconnected socket */
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*SSL_accept failed*");
    g_assert_cmpint(socketpair(AF_LOCAL, SOCK_STREAM, 0, sv), ==, 0);
    close(sv[1]);
    result = spice_server_add_ssl_client(server, sv[0], 1);
    g_assert_cmpint(result, ==, -1);
    /* if the function fails, it should not close the socket */
    g_assert_cmpint(close(sv[0]), ==, 0);

    spice_server_destroy(server);
    basic_event_loop_destroy();
}

static int vmc_write(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                     SPICE_GNUC_UNUSED const uint8_t *buf,
                     int len)
{
    return len;
}

static int vmc_read(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                    SPICE_GNUC_UNUSED uint8_t *buf,
                    SPICE_GNUC_UNUSED int len)
{
    return 0;
}

static void vmc_state(SPICE_GNUC_UNUSED SpiceCharDeviceInstance *sin,
                      SPICE_GNUC_UNUSED int connected)
{
}

static SpiceCharDeviceInterface vmc_interface = {
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

static SpiceCharDeviceInstance vmc_instance;

static void vmc_leaks(void)
{
    SpiceCoreInterface *core = basic_event_loop_init();
    Test *test = test_new(core);
    int status;

    vmc_instance.subtype = "usbredir";
    vmc_instance.base.sif = &vmc_interface.base;
    spice_server_add_interface(test->server, &vmc_instance.base);
    status = spice_server_remove_interface(&vmc_instance.base);
    g_assert_cmpint(status, ==, 0);

    vmc_instance.subtype = "port";
    vmc_instance.portname = "org.spice-space.webdav.0";
    vmc_instance.base.sif = &vmc_interface.base;
    spice_server_add_interface(test->server, &vmc_instance.base);
    status = spice_server_remove_interface(&vmc_instance.base);
    g_assert_cmpint(status, ==, 0);

    vmc_instance.subtype = "port";
    vmc_instance.portname = "default_port";
    vmc_instance.base.sif = &vmc_interface.base;
    spice_server_add_interface(test->server, &vmc_instance.base);
    status = spice_server_remove_interface(&vmc_instance.base);
    g_assert_cmpint(status, ==, 0);

    test_destroy(test);
    basic_event_loop_destroy();
}

static void migrate_cb(SpiceMigrateInstance *sin)
{
}

static const SpiceMigrateInterface migrate_interface = {
    .base = {
        .type          = SPICE_INTERFACE_MIGRATION,
        .description   = "migration",
        .major_version = SPICE_INTERFACE_MIGRATION_MAJOR,
        .minor_version = SPICE_INTERFACE_MIGRATION_MINOR,
    },
    .migrate_connect_complete = migrate_cb,
    .migrate_end_complete = migrate_cb,
};

static void migration_leaks(void)
{
    SpiceCoreInterface *core;
    SpiceServer *server = spice_server_new();
    SpiceMigrateInstance migrate;

    g_assert_nonnull(server);

    core = basic_event_loop_init();
    g_assert_nonnull(core);

    g_assert_cmpint(spice_server_init(server, core), ==, 0);

    migrate.base.sif = &migrate_interface.base;
    spice_server_add_interface(server, &migrate.base);

    spice_server_destroy(server);
    basic_event_loop_destroy();
}

static void tablet_set_logical_size(SpiceTabletInstance* sin, int width, int height)
{
}

static void tablet_position(SpiceTabletInstance* sin, int x, int y,
                            uint32_t buttons_state)
{
}

static void tablet_wheel(SpiceTabletInstance* sin, int wheel,
                         uint32_t buttons_state)
{
}

static void tablet_buttons(SpiceTabletInstance *sin,
                           uint32_t buttons_state)
{
}

static const SpiceTabletInterface tablet_interface = {
    .base = {
        .type          = SPICE_INTERFACE_TABLET,
        .description   = "tablet",
        .major_version = SPICE_INTERFACE_TABLET_MAJOR,
        .minor_version = SPICE_INTERFACE_TABLET_MINOR,
    },
    .set_logical_size   = tablet_set_logical_size,
    .position           = tablet_position,
    .wheel              = tablet_wheel,
    .buttons            = tablet_buttons,
};

static void tablet_leaks(void)
{
    SpiceCoreInterface *core;
    SpiceServer *server;
    SpiceTabletInstance tablet;

    core = basic_event_loop_init();
    g_assert_nonnull(core);

    // test if leaks without spice_server_remove_interface
    server = spice_server_new();
    g_assert_nonnull(server);
    g_assert_cmpint(spice_server_init(server, core), ==, 0);

    tablet.base.sif = &tablet_interface.base;
    spice_server_add_interface(server, &tablet.base);

    spice_server_destroy(server);

    // test if leaks with spice_server_remove_interface
    server = spice_server_new();
    g_assert_nonnull(server);
    g_assert_cmpint(spice_server_init(server, core), ==, 0);

    tablet.base.sif = &tablet_interface.base;
    spice_server_add_interface(server, &tablet.base);
    spice_server_remove_interface(&tablet.base);

    spice_server_destroy(server);

    basic_event_loop_destroy();
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/server/server leaks", server_leaks);
    g_test_add_func("/server/vmc leaks", vmc_leaks);
    g_test_add_func("/server/migration leaks", migration_leaks);
    g_test_add_func("/server/tablet leaks", tablet_leaks);

    return g_test_run();
}
