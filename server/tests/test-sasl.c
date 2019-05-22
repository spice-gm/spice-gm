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
 * Test SASL connections.
 */
#include <config.h>

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <spice.h>
#include <sasl/sasl.h>

#include <spice/protocol.h>
#include <common/macros.h>

#include "test-glib-compat.h"
#include "basic-event-loop.h"

#include <spice/start-packed.h>
typedef struct SPICE_ATTR_PACKED SpiceInitialMessage {
        SpiceLinkHeader hdr;
        SpiceLinkMess mess;
        uint32_t caps[2];
} SpiceInitialMessage;
#include <spice/end-packed.h>

typedef enum {
    FLAG_NONE = 0,
    FLAG_START_OK = 1,
    FLAG_LOW_SSF = 2,
    FLAG_SERVER_NULL_START = 4,
    FLAG_SERVER_NULL_STEP = 8,
    FLAG_CLIENT_NULL_START = 16,
    FLAG_CLIENT_NULL_STEP = 32,
    FLAG_SERVER_BIG_START = 64,
    FLAG_SERVER_BIG_STEP = 128,
    FLAG_CLIENT_BIG_START = 256,
    FLAG_CLIENT_BIG_STEP = 512,
    FLAG_START_ERROR = 1024,
    FLAG_STEP_ERROR = 2048,
} TestFlags;

static char *mechlist;
static char *big_data;
static bool mechlist_called;
static bool start_called;
static bool step_called;
static bool encode_called;
static unsigned test_flags;

static SpiceCoreInterface *core;
static SpiceServer *server;

static unsigned int test_num;

static gboolean idle_end_test(void *arg);

static void
check_sasl_conn(sasl_conn_t *conn)
{
    g_assert_nonnull(conn);
}

static void
get_step_out(const char **serverout, unsigned *serveroutlen,
             const char *normal_data, unsigned null_flag, unsigned big_flag)
{
    if ((test_flags & big_flag) != 0) {
        *serverout = big_data;
        *serveroutlen = strlen(big_data);
    } else if ((test_flags & null_flag) != 0) {
        *serverout = NULL;
        *serveroutlen = 0;
    } else {
        *serverout = normal_data;
        *serveroutlen = strlen(normal_data);
    }
}

int
sasl_server_init(const sasl_callback_t *callbacks, const char *appname)
{
    g_assert_null(callbacks);
    return SASL_OK;
}

int
sasl_decode(sasl_conn_t *conn,
            const char *input, unsigned inputlen,
            const char **output, unsigned *outputlen)
{
    check_sasl_conn(conn);
    g_assert(start_called);

    return SASL_NOTDONE;
}

int
sasl_encode(sasl_conn_t *conn,
            const char *input, unsigned inputlen,
            const char **output, unsigned *outputlen)
{
    check_sasl_conn(conn);
    g_assert(start_called);

    encode_called = true;
    return SASL_NOTDONE;
}

const char *
sasl_errdetail(sasl_conn_t *conn)
{
    check_sasl_conn(conn);
    return "XXX";
}

const char *
sasl_errstring(int saslerr,
               const char *langlist,
               const char **outlang)
{
    return "YYY";
}

int
sasl_getprop(sasl_conn_t *conn, int propnum,
             const void **pvalue)
{
    check_sasl_conn(conn);
    g_assert_nonnull(pvalue);

    if (propnum == SASL_SSF) {
        static int val;
        val = (test_flags & FLAG_LOW_SSF) ? 44 : 64;
        *pvalue = &val;
    }
    return SASL_OK;
}

int
sasl_setprop(sasl_conn_t *conn,
             int propnum,
             const void *value)
{
    check_sasl_conn(conn);
    g_assert(value);
    return SASL_OK;
}

int
sasl_server_new(const char *service,
                const char *serverFQDN,
                const char *user_realm,
                const char *iplocalport,
                const char *ipremoteport,
                const sasl_callback_t *callbacks,
                unsigned flags,
                sasl_conn_t **pconn)
{
    g_assert_nonnull(pconn);
    g_assert_null(callbacks);

    *pconn = (sasl_conn_t *) GUINT_TO_POINTER(0xdeadbeef);
    return SASL_OK;
}

void
sasl_dispose(sasl_conn_t **pconn)
{
    g_assert_nonnull(pconn);
    check_sasl_conn(*pconn);
}

int
sasl_listmech(sasl_conn_t *conn,
              const char *user,
              const char *prefix,
              const char *sep,
              const char *suffix,
              const char **result,
              unsigned *plen,
              int *pcount)
{
    check_sasl_conn(conn);
    g_assert_nonnull(result);
    g_assert_nonnull(prefix);
    g_assert_nonnull(sep);
    g_assert_nonnull(suffix);
    g_assert(!mechlist_called);
    g_assert(!start_called);
    g_assert(!step_called);
    mechlist_called = true;

    g_free(mechlist);
    mechlist = g_strjoin("", prefix, "ONE", sep, "TWO", sep, "THREE", suffix, NULL);
    *result = mechlist;
    return SASL_OK;
}

int
sasl_server_start(sasl_conn_t *conn,
                  const char *mech,
                  const char *clientin,
                  unsigned clientinlen,
                  const char **serverout,
                  unsigned *serveroutlen)
{
    check_sasl_conn(conn);
    g_assert_nonnull(serverout);
    g_assert(mechlist_called);
    g_assert(!start_called);
    g_assert(!step_called);
    start_called = true;

    get_step_out(serverout, serveroutlen, "foo", FLAG_SERVER_NULL_START, FLAG_SERVER_BIG_START);
    if (test_flags & FLAG_START_ERROR) {
        return SASL_FAIL;
    }
    return (test_flags & FLAG_START_OK) ? SASL_OK : SASL_CONTINUE;
}

int
sasl_server_step(sasl_conn_t *conn,
                 const char *clientin,
                 unsigned clientinlen,
                 const char **serverout,
                 unsigned *serveroutlen)
{
    check_sasl_conn(conn);
    g_assert_nonnull(serverout);
    g_assert(start_called);
    step_called = true;

    get_step_out(serverout, serveroutlen, "foo", FLAG_SERVER_NULL_STEP, FLAG_SERVER_BIG_STEP);
    if (test_flags & FLAG_STEP_ERROR) {
        return SASL_FAIL;
    }
    return SASL_OK;
}

static SpiceInitialMessage initial_message = {
    {
        0, // SPICE_MAGIC,
        GUINT32_TO_LE(SPICE_VERSION_MAJOR), GUINT32_TO_LE(SPICE_VERSION_MINOR),
        GUINT32_TO_LE(sizeof(SpiceInitialMessage) - sizeof(SpiceLinkHeader))
    },
    {
        0,
        SPICE_CHANNEL_MAIN,
        0,
        GUINT32_TO_LE(1),
        GUINT32_TO_LE(1),
        GUINT32_TO_LE(sizeof(SpiceLinkMess))
    },
    {
        GUINT32_TO_LE(SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION|SPICE_COMMON_CAP_AUTH_SASL|
                      SPICE_COMMON_CAP_MINI_HEADER),
        0
    }
};

static void
reset_test(void)
{
    mechlist_called = false;
    start_called = false;
    step_called = false;
    encode_called = false;
    test_flags = FLAG_NONE;
}

static void
start_test(void)
{
    g_assert_null(big_data);
    big_data = (char*) g_malloc(1024 * 1024 + 10);
    for (unsigned n = 0; n < 1024 * 1024 + 10; ++n) {
        big_data[n] = ' ' + (n % 94);
    }
    big_data[1024 * 1024 + 5] = 0;

    g_assert_null(server);

    initial_message.hdr.magic = SPICE_MAGIC;

    reset_test();

    core = basic_event_loop_init();
    g_assert_nonnull(core);

    server = spice_server_new();
    g_assert_nonnull(server);
    spice_server_set_sasl(server, true);
    g_assert_cmpint(spice_server_init(server, core), ==, 0);
}

static void
end_tests(void)
{
    spice_server_destroy(server);
    server = NULL;

    basic_event_loop_destroy();
    core = NULL;

    g_free(mechlist);
    mechlist = NULL;

    g_free(big_data);
    big_data = NULL;
}

static size_t
do_readwrite_all(int fd, const void *buf, const size_t len, bool do_write)
{
    size_t byte_count = 0;
    while (byte_count < len) {
        int l;
        if (do_write) {
            l = write(fd, (const char *) buf + byte_count, len - byte_count);
        } else {
            l = read(fd, (char *) buf + byte_count, len - byte_count);
            if (l == 0) {
                return byte_count;
            }
        }
        if (l < 0 && errno == EINTR) {
            continue;
        }
        if (l < 0) {
            return l;
        }
        byte_count += l;
    }
    return byte_count;
}

// use macro to maintain line number on error
#define read_all(fd, buf, len) \
    g_assert_cmpint(do_readwrite_all(fd, buf, len, false), ==, len)

#define write_all(fd, buf, len) \
    g_assert_cmpint(do_readwrite_all(fd, buf, len, true), ==, len)

static ssize_t
read_u32_err(int fd, uint32_t *out)
{
    uint32_t val = 0;
    ssize_t ret = do_readwrite_all(fd, &val, sizeof(val), false);
    *out = GUINT32_FROM_LE(val);
    return ret;
}
#define read_u32(fd, out) \
    g_assert_cmpint(read_u32_err(fd, out), ==, sizeof(uint32_t))

static ssize_t
write_u32_err(int fd, uint32_t val)
{
    val = GUINT32_TO_LE(val);
    return do_readwrite_all(fd, &val, sizeof(val), true);
}

#define write_u32(fd, val) \
    g_assert_cmpint(write_u32_err(fd, val), ==, sizeof(uint32_t))

/* This function is similar to g_idle_add but uses our internal Glib
 * main context. g_idle_add uses the default main context but to make
 * sure we can use a different main context we don't use the default
 * one (as Qemu does) */
static void
idle_add(GSourceFunc func, void *arg)
{
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, func, NULL, NULL);
    g_source_attach(source, basic_event_loop_get_context());
    g_source_unref(source);
}

typedef enum {
    STEP_NONE,
    STEP_READ_MECHLIST_LEN,
    STEP_READ_MECHLIST,
    STEP_WRITE_MECHNAME_LEN,
    STEP_WRITE_MECHNAME,
    STEP_WRITE_START_LEN,
    STEP_WRITE_START,
    STEP_WRITE_STEP_LEN,
    STEP_WRITE_STEP,
    STEP_NEVER,
} ClientEmulationSteps;

typedef enum {
    FAILURE_OK,
    FAILURE_UNKNOWN,
    FAILURE_DISCONNECT,
    FAILURE_MAGIC,
    FAILURE_AUTH,
} FailureType;

typedef struct {
    const char *mechname;
    int mechlen;
    bool success;
    FailureType failure_type;
    ClientEmulationSteps last_step;
    unsigned flags;
    int line;
} TestData;

static char long_mechname[128];
static TestData tests_data[] = {
    // these should just succeed
#define TEST_SUCCESS(mech) \
    { mech, -1, true, FAILURE_OK, STEP_NEVER, FLAG_NONE, __LINE__ },
    TEST_SUCCESS("ONE")
    TEST_SUCCESS("TWO")
    TEST_SUCCESS("THREE")

    // these test bad mech names
#define TEST_BAD_NAME(mech, len) \
    { mech, len, false, FAILURE_MAGIC, STEP_NEVER, FLAG_NONE, __LINE__ },
#define TEST_BAD_NAME_LEN(mech, len) \
    { mech, len, false, FAILURE_DISCONNECT, STEP_NEVER, FLAG_NONE, __LINE__ },
    TEST_BAD_NAME("ON", -1)
    TEST_BAD_NAME("NE", -1)
    TEST_BAD_NAME("THRE", -1)
    TEST_BAD_NAME("HREE", -1)
    TEST_BAD_NAME("ON\x00", 3)
    TEST_BAD_NAME("O\x00\x00", 3)
    TEST_BAD_NAME_LEN("", -1)
    TEST_BAD_NAME(long_mechname, 100)
    TEST_BAD_NAME_LEN(long_mechname, 101)
    TEST_BAD_NAME("ONE,TWO", -1)

    // stop before filling everything
#define TEST_EARLY_STOP(step) \
    { "ONE", -1, false, FAILURE_UNKNOWN, step, FLAG_NONE, __LINE__},
    TEST_EARLY_STOP(STEP_READ_MECHLIST_LEN)
    TEST_EARLY_STOP(STEP_READ_MECHLIST)
    TEST_EARLY_STOP(STEP_WRITE_MECHNAME_LEN)
    TEST_EARLY_STOP(STEP_WRITE_MECHNAME)
    TEST_EARLY_STOP(STEP_WRITE_START_LEN)
    TEST_EARLY_STOP(STEP_WRITE_START)
    TEST_EARLY_STOP(STEP_WRITE_STEP_LEN)

#define TEST_FLAGS_OK(flags) \
    { "ONE", -1, true, FAILURE_OK, STEP_NEVER, flags, __LINE__},
#define TEST_FLAGS_KO(failure, flags) \
    { "ONE", -1, false, failure, STEP_NEVER, flags, __LINE__},
    TEST_FLAGS_KO(FAILURE_OK, FLAG_LOW_SSF)
    TEST_FLAGS_KO(FAILURE_OK, FLAG_START_OK|FLAG_LOW_SSF)
    TEST_FLAGS_OK(FLAG_START_OK)
    TEST_FLAGS_OK(FLAG_SERVER_NULL_START)
    TEST_FLAGS_OK(FLAG_SERVER_NULL_STEP)
    TEST_FLAGS_OK(FLAG_CLIENT_NULL_START)
    TEST_FLAGS_OK(FLAG_CLIENT_NULL_STEP)
    TEST_FLAGS_KO(FAILURE_DISCONNECT, FLAG_SERVER_BIG_START)
    TEST_FLAGS_KO(FAILURE_DISCONNECT, FLAG_SERVER_BIG_STEP)
    TEST_FLAGS_KO(FAILURE_MAGIC, FLAG_CLIENT_BIG_START)
    TEST_FLAGS_KO(FAILURE_DISCONNECT, FLAG_CLIENT_BIG_STEP)
    TEST_FLAGS_KO(FAILURE_DISCONNECT, FLAG_START_ERROR)
    TEST_FLAGS_KO(FAILURE_DISCONNECT, FLAG_STEP_ERROR)
};

static FailureType
client_emulator(int sock)
{
    const TestData *data = &tests_data[test_num];

#define STOP_AT(step) if (data->last_step == STEP_ ## step) { return FAILURE_UNKNOWN; }

    // send initial message
    write_all(sock, &initial_message, sizeof(initial_message));

    // server replies link ack with rsa, etc, similar to above beside
    // fixed fields
    struct {
        SpiceLinkHeader header;
        SpiceLinkReply ack;
    } msg;
    SPICE_VERIFY(sizeof(msg) == sizeof(SpiceLinkHeader) + sizeof(SpiceLinkReply));
    read_all(sock, &msg, sizeof(msg));
    uint32_t num_caps = GUINT32_FROM_LE(msg.ack.num_common_caps) +
                        GUINT32_FROM_LE(msg.ack.num_channel_caps);
    while (num_caps-- > 0) {
        uint32_t cap;
        read_all(sock, &cap, sizeof(cap));
    }

    // client have to send a SpiceLinkAuthMechanism (just uint32 with
    // mech SPICE_COMMON_CAP_AUTH_SASL)
    write_u32(sock, SPICE_COMMON_CAP_AUTH_SASL);

    STOP_AT(NONE);

    // sasl finally start, data starts from server (mech list)
    //
    uint32_t mechlen;
    read_u32(sock, &mechlen);
    STOP_AT(READ_MECHLIST_LEN);

    char buf[300];
    g_assert_cmpint(mechlen, <=, sizeof(buf));
    read_all(sock, buf, mechlen);
    STOP_AT(READ_MECHLIST);

    // mech name
    write_u32(sock, data->mechlen);
    STOP_AT(WRITE_MECHNAME_LEN);
    write_all(sock, data->mechname, data->mechlen);
    STOP_AT(WRITE_MECHNAME);

    // first challenge
    const char *out;
    unsigned outlen;
    get_step_out(&out, &outlen, "START", FLAG_CLIENT_NULL_START, FLAG_CLIENT_BIG_START);
    if (write_u32_err(sock, out ? outlen : 0) == sizeof(uint32_t)) {
        STOP_AT(WRITE_START_LEN);
        if (out) {
            do_readwrite_all(sock, out, outlen, true);
        }
        STOP_AT(WRITE_START);
    }

    for (;;) {
        uint32_t datalen;
        if (read_u32_err(sock, &datalen) != sizeof(datalen)) {
            return FAILURE_DISCONNECT;
        }
        if (datalen == GUINT32_FROM_LE(SPICE_MAGIC)) {
            return FAILURE_MAGIC;
        }
        g_assert_cmpint(datalen, <=, sizeof(buf));
        read_all(sock, buf, datalen);

        uint8_t is_ok = 0;
        read_all(sock, &is_ok, sizeof(is_ok));
        if (is_ok) {
            // is_ok should be 0 or 1
            g_assert_cmpint(is_ok, ==, 1);
            uint32_t step_result;
            read_u32(sock, &step_result);
            if (!step_result) {
                return FAILURE_AUTH;
            }
            return FAILURE_OK;
        }

        get_step_out(&out, &outlen, "STEP", FLAG_CLIENT_NULL_STEP, FLAG_CLIENT_BIG_STEP);
        if (write_u32_err(sock, out ? outlen : 0) == sizeof(uint32_t)) {
            STOP_AT(WRITE_STEP_LEN);
            if (out) {
                do_readwrite_all(sock, out, outlen, true);
            }
            STOP_AT(WRITE_STEP);
        }
    }
}

static void *
client_emulator_thread(void *arg)
{
    int sock = GPOINTER_TO_INT(arg);

    FailureType result = client_emulator(sock);

    shutdown(sock, SHUT_RDWR);
    close(sock);

    idle_add(idle_end_test, NULL);

    return GINT_TO_POINTER(result);
}

static pthread_t
setup_thread(void)
{
    TestData *data = &tests_data[test_num];
    if (data->mechlen < 0) {
        data->mechlen = strlen(data->mechname);
    }
    test_flags = data->flags;
    int len = data->mechlen;
    printf("\nRunning test %d ('%.*s' %d) line %d\n",
           test_num, len, data->mechname, len, data->line);

    int sv[2];
    g_assert_cmpint(socketpair(AF_LOCAL, SOCK_STREAM, 0, sv), ==, 0);

    g_assert(spice_server_add_client(server, sv[0], 0) == 0);

    pthread_t thread;
    g_assert_cmpint(pthread_create(&thread, NULL, client_emulator_thread,
                                   GINT_TO_POINTER(sv[1])), ==, 0);
    return thread;
}

// called when the next test has to be run
static gboolean
idle_end_test(void *arg)
{
    basic_event_loop_quit();

    return G_SOURCE_REMOVE;
}

static void
check_test_results(FailureType res)
{
    const TestData *data = &tests_data[test_num];
    if (data->success) {
        g_assert(encode_called);
        return;
    }

    g_assert(mechlist_called);
    g_assert(!encode_called);
    g_assert_cmpint(res, ==, data->failure_type);
}

static void
sasl_mechs(void)
{
    start_test();

    memset(long_mechname, 'X', sizeof(long_mechname));

    for (test_num = 0; test_num < G_N_ELEMENTS(tests_data); test_num++) {
        pthread_t thread = setup_thread();
        alarm(4);
        basic_event_loop_mainloop();
        void *thread_ret = NULL;
        g_assert_cmpint(pthread_join(thread, &thread_ret), ==, 0);
        alarm(0);
        check_test_results((FailureType)GPOINTER_TO_INT(thread_ret));
        reset_test();
    }

    end_tests();
}

int
main(int argc, char *argv[])
{
    sasl_mechs();

    return 0;
}
