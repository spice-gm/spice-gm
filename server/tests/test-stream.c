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
#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>

#include <common/log.h>
#include "red-stream.h"
#include "basic-event-loop.h"

static SpiceServer *server = NULL;

static int server_init(void)
{
    SpiceCoreInterface *core = basic_event_loop_init();
    server = spice_server_new();

    return spice_server_init(server, core);
}


/*
 * Based on code from Keith Packard:
 * http://keithp.com/blogs/fd-passing/
 */
static ssize_t
sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd)
{
    ssize_t size;

    if (fd) {
        struct msghdr msg;
        struct iovec iov;
        union {
            struct cmsghdr cmsghdr;
            char control[CMSG_SPACE(sizeof (int))];
        } cmsgu;
        struct cmsghdr *cmsg;

        iov.iov_base = buf;
        iov.iov_len = bufsize;

        msg.msg_name = NULL;
        msg.msg_namelen = 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);
        msg.msg_flags = 0;
        size = recvmsg(sock, &msg, 0);
        if (size < 0) {
            perror ("recvmsg");
            exit(1);
        }
        cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            if (cmsg->cmsg_level != SOL_SOCKET) {
                fprintf(stderr, "invalid cmsg_level %d\n",
                        cmsg->cmsg_level);
                exit(1);
            }
            if (cmsg->cmsg_type != SCM_RIGHTS) {
                fprintf(stderr, "invalid cmsg_type %d\n",
                        cmsg->cmsg_type);
                exit(1);
            }

            memcpy(fd, CMSG_DATA(cmsg), sizeof(*fd));
        } else
            *fd = -1;
    } else {
        size = read(sock, buf, bufsize);
        if (size < 0) {
            perror("read");
            exit(1);
        }
    }

    return size;
}

int main(int argc, char *argv[])
{
    RedStream *st[2];
    int sv[2];
    int ret, fd = -1;
    char c;

    spice_return_val_if_fail(server_init() == 0, -1);

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) == -1) {
        spice_error("socketpair failed %s", strerror(errno));
        return -1;
    }

    st[0] = red_stream_new(server, sv[0]);
    spice_assert(red_stream_is_plain_unix(st[0]));
    st[1] = red_stream_new(server, sv[1]);
    spice_assert(red_stream_is_plain_unix(st[1]));

    /* send stdin, for the fun of it */
    ret = red_stream_send_msgfd(st[0], 0);
    spice_assert(ret == 1);
    ret = sock_fd_read(sv[1], &c, 1, &fd);
    spice_assert(c == '@');
    spice_assert(ret == 1);
    spice_assert(fd != -1);
    close(fd);

    /* send invalid fd behaviour */
    ret = red_stream_send_msgfd(st[0], -1);
    spice_assert(ret == 1);
    ret = sock_fd_read(sv[1], &c, 1, &fd);
    spice_assert(c == '@');
    spice_assert(ret == 1);
    spice_assert(fd == -1);

    /* batch test */
    ret = red_stream_send_msgfd(st[0], 0);
    spice_assert(ret == 1);
    ret = red_stream_send_msgfd(st[0], 0);
    spice_assert(ret == 1);
    ret = sock_fd_read(sv[1], &c, 1, &fd);
    spice_assert(c == '@');
    spice_assert(ret == 1);
    spice_assert(fd != -1);
    close(fd);
    ret = sock_fd_read(sv[1], &c, 1, &fd);
    spice_assert(c == '@');
    spice_assert(ret == 1);
    spice_assert(fd != -1);
    close(fd);

    red_stream_free(st[0]);
    red_stream_free(st[1]);

    return 0;
}
