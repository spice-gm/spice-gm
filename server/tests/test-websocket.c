/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2017-2019 Red Hat, Inc.

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
/* Utility to allow checking our websocket implementaion using Autobahn
 * Test Suite.
 * This suite require a WebSocket server implementation echoing
 * data sent to it
 */
#undef NDEBUG
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <err.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <glib.h>
#include <signal.h>

#include "net-utils.h"
#include "websocket.h"

/*
on data arrived on socket:
  try to read data, read again till error, handle error, on EAGAIN polling again
  queue readed data for echo

on data writable (if we have data to write):
  write data

question... pings are handled ??

if data size == 0 when we receive we must send it

*/

static int port = 7777;
static gboolean non_blocking = false;
static gboolean debug = false;
static volatile bool got_term = false;
static unsigned int num_connections = 0;

static GOptionEntry cmd_entries[] = {
  {"port", 'p', 0, G_OPTION_ARG_INT, &port,
   "Local port to bind to", NULL},
  {"non-blocking", 'n', 0, G_OPTION_ARG_NONE, &non_blocking,
   "Enable non-blocking i/o", NULL},
  {"debug", 0, 0, G_OPTION_ARG_NONE, &debug,
   "Enable debug output", NULL},
  {NULL}
};

static void handle_client(int new_sock);

static int
wait_for(int sock, short events)
{
    struct pollfd fds[1] = { { sock, events, 0 } };
    for (;;) {
        switch (poll(fds, 1, -1)) {
        case -1:
            if (errno == EINTR) {
                if (got_term) {
                    printf("handled %u connections\n", num_connections);
                    exit(0);
                }
                break;
            }
            err(1, "poll");
            break;
        case 1:
            if ((fds->revents & events) != 0) {
                return fds->revents & events;
            }
            break;
        case 0:
            assert(0);
        }
    }
}

static ssize_t
ws_read(void *opaque, void *buf, size_t nbyte)
{
    int sock = GPOINTER_TO_INT(opaque);
    return recv(sock, buf, nbyte, MSG_NOSIGNAL);
}

static ssize_t
ws_write(void *opaque, const void *buf, size_t nbyte)
{
    int sock = GPOINTER_TO_INT(opaque);
    return send(sock, buf, nbyte, MSG_NOSIGNAL);
}

static ssize_t
ws_writev(void *opaque, struct iovec *iov, int iovcnt)
{
    int sock = GPOINTER_TO_INT(opaque);
    return writev(sock, iov, iovcnt);
}

static void
go_out(int sig)
{
    got_term = true;
}

int
main(int argc, char **argv)
{
    GError *error = NULL;
    GOptionContext *context;

    context = g_option_context_new(" - Websocket test");
    g_option_context_add_main_entries(context, cmd_entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        errx(1, "%s: %s\n", argv[0], error->message);
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        err(1, "socket");
    }

    int enable = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   (const void *) &enable, sizeof(enable)) < 0) {
        err(1, "setsockopt reuseaddr");
    }

    if (non_blocking) {
        red_socket_set_non_blocking(sock, true);
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons((short) port);
    sin.sin_family = AF_INET;

    if (bind(sock, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
        err(1, "bind");
    }

    if (listen(sock, 5) < 0) {
        err(1, "listen");
    }

    signal(SIGTERM, go_out);
    signal(SIGINT, go_out);

    while (!got_term) {
        wait_for(sock, POLLIN);

        socklen_t sock_len = sizeof(sin);
        int new_sock = accept(sock, (struct sockaddr *) &sin, &sock_len);
        if (got_term) {
            break;
        }
        if (new_sock < 0) {
            err(1, "accept");
        }

        ++num_connections;
        handle_client(new_sock);

        socket_close(new_sock);
    }

    socket_close(sock);
    printf("handled %u connections\n", num_connections);
    return 0;
}

static void
handle_client(int new_sock)
{
    if (non_blocking) {
        red_socket_set_non_blocking(new_sock, true);
    }

    int enable = 1;
    if (setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY,
                   (const void *) &enable, sizeof(enable)) < 0) {
        err(1, "setsockopt nodelay");
    }

    // wait header
    wait_for(new_sock, POLLIN);

    RedsWebSocket *ws = websocket_new("", 0, GINT_TO_POINTER(new_sock),
                                      ws_read, ws_write, ws_writev);
    assert(ws);

    char buffer[4096];
    bool got_message = false;
    size_t to_send = 0;
    unsigned ws_flags = WEBSOCKET_BINARY_FINAL;
    while (!got_term) {
        int events = 0;
        if (sizeof(buffer) > to_send &&
            (!got_message || (ws_flags & WEBSOCKET_FINAL) == 0)) {
            events |= POLLIN;
        }
        assert(!to_send || got_message);
        if (got_message) {
            events |= POLLOUT;
        }
        events = wait_for(new_sock, events);
        if (events & POLLIN) {
            assert(sizeof(buffer) > to_send);
            unsigned flags;
            int size = websocket_read(ws, (void *) (buffer + to_send), sizeof(buffer) - to_send,
                                      &flags);
            if (flags) {
                ws_flags = flags;
            }

            if (size < 0) {
                if (errno == EIO) {
                    break;
                }
                if (errno == EAGAIN) {
                    continue;
                }
                err(1, "recv");
            }

            if (size == 0 && flags == 0) {
                break;
            }

            if (debug) {
                printf("received %d bytes of data flags %x\n", size, flags);
            }
            to_send += size;
            got_message = true;
        }

        if (events & POLLOUT) {
            int size = websocket_write(ws, buffer, to_send, ws_flags);

            if (size < 0) {
                switch (errno) {
                case EAGAIN:
                case EINTR:
                    continue;
                case ECONNRESET:
                    break;
                default:
                    err(1, "send");
                }
                break;
            }

            if (debug) {
                printf("sent %d bytes of data\n", size);
            }

            to_send -= size;
            memmove(buffer, buffer + size, to_send);
            if (!to_send) {
                got_message = false;
            }
        }
    }

    websocket_free(ws);

    if (debug) {
        printf("connection closed\n");
    }
}
