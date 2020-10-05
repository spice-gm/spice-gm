/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2020 Red Hat, Inc.

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
/**
 * Test Dispatcher class and speed
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>

#include "basic-event-loop.h"
#include "test-glib-compat.h"
#include "reds.h"
#include "dispatcher.h"
#include "win-alarm.h"

// iterations to run for each test, useful also to check the speed
static unsigned iterations = 100;

static SpiceCoreInterface *core;
static SpiceCoreInterfaceInternal core_int;
static red::shared_ptr<Dispatcher> dispatcher;
static SpiceWatch *watch;
// incremental number we use during the test, each message sent is incremented
static unsigned num;
using TestFixture = int;

static void test_dispatcher_setup(TestFixture *fixture, gconstpointer user_data)
{
    num = 0;
    dispatcher.reset();
    g_assert_null(core);
    core = basic_event_loop_init();
    g_assert_nonnull(core);
    core_int = core_interface_adapter;
    core_int.public_interface = core;
    dispatcher = red::make_shared<Dispatcher>(10);
    // TODO not create Reds, just the internal interface ??
    watch = dispatcher->create_watch(&core_int);
}

static void test_dispatcher_teardown(TestFixture *fixture, gconstpointer user_data)
{
    g_assert_nonnull(core);

    red_watch_remove(watch);
    watch = nullptr;
    dispatcher.reset();
    basic_event_loop_destroy();
    core = nullptr;
}

// test message to sent
struct Msg {
    uint64_t num;
    void *dummy;
};

// message handler to mark stop, stop the event loop
static void msg_end(void *, Msg *msg)
{
    g_assert_cmpint(num, ==, iterations);
    basic_event_loop_quit();
}

// message handler to check message number
static void msg_check(void *, Msg *msg)
{
    g_assert_cmpint(msg->num, ==, num);
    ++num;
}

static void *thread_proc(void *arg)
{
    // the argument is number of messages with NACK to send
    int n_nack = GPOINTER_TO_INT(arg);
    g_assert_cmpint(n_nack, >=, 0);
    g_assert_cmpint(n_nack, <=, 10);

    auto start = spice_get_monotonic_time_ns();

    // repeat sending messages
    for (unsigned n = 0; n < iterations; ++n) {
        Msg msg{n, nullptr};
        dispatcher->send_message_custom(msg_check, &msg, (n % 10) >= n_nack);
    }

    // one last sync to wait
    Msg msg{0, nullptr};
    dispatcher->send_message_custom(msg_end, &msg, true);

    // measure time
    auto cost = spice_get_monotonic_time_ns() - start;

    printf("With ACK/NACK %d/%d time spent %gus each over %u iterations\n",
           10 - n_nack, n_nack,
           cost / 1000.0 / iterations, iterations);
    return nullptr;
}

static void test_dispatcher(TestFixture *fixture, gconstpointer user_data)
{
    pthread_t th;

    g_assert_cmpint(pthread_create(&th, nullptr, thread_proc, (void *) user_data), ==, 0);

    // start all test
    alarm(20);
    basic_event_loop_mainloop();
    alarm(0);

    pthread_join(th, nullptr);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    // override number of iteration passing a parameter
    if (argc >= 2 && atoi(argv[1]) > 10) {
        iterations = atoi(argv[1]);
    }

    for (int i = 0; i <= 10; ++i) {
        char name[64];
        sprintf(name, "/server/dispatcher/%d", i);
        g_test_add(name, TestFixture, GINT_TO_POINTER(i), test_dispatcher_setup,
                   test_dispatcher, test_dispatcher_teardown);
    }

    return g_test_run();
}
