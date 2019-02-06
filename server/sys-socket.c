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

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <common/log.h>

#include "sys-socket.h"

#ifdef _WIN32
// Map Windows socket errors to standard C ones
// See https://msdn.microsoft.com/en-us/library/windows/desktop/ms740668(v=vs.85).aspx
void socket_win32_set_errno(void)
{
    int err = EPIPE; // default
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK:
    case WSAEINPROGRESS:
        err = EAGAIN;
        break;
    case WSAEINTR:
        err = EINTR;
        break;
    case WSAEBADF:
        err = EBADF;
        break;
    case WSA_INVALID_HANDLE:
    case WSA_INVALID_PARAMETER:
    case WSAEINVAL:
        err = EINVAL;
        break;
    case WSAENOTSOCK:
        err = ENOTSOCK;
        break;
    case WSA_NOT_ENOUGH_MEMORY:
        err = ENOMEM;
        break;
    case WSAEPROTONOSUPPORT:
    case WSAESOCKTNOSUPPORT:
    case WSAEOPNOTSUPP:
    case WSAEPFNOSUPPORT:
    case WSAEAFNOSUPPORT:
    case WSAVERNOTSUPPORTED:
        err = ENOTSUP;
        break;
    case WSAEFAULT:
        err = EFAULT;
        break;
    case WSAEACCES:
        err = EACCES;
        break;
    case WSAEMFILE:
        err = EMFILE;
        break;
    case WSAENAMETOOLONG:
        err = ENAMETOOLONG;
        break;
    case WSAENOTEMPTY:
        err = ENOTEMPTY;
        break;
    case WSA_OPERATION_ABORTED:
    case WSAECANCELLED:
    case WSA_E_CANCELLED:
        err = ECANCELED;
        break;
    case WSAEADDRINUSE:
        err = EADDRINUSE;
        break;
    case WSAENETDOWN:
        err = ENETDOWN;
        break;
    case WSAENETUNREACH:
        err = ENETUNREACH;
        break;
    case WSAENETRESET:
        err = ENETRESET;
        break;
    case WSAECONNABORTED:
        err = ECONNABORTED;
        break;
    case WSAECONNRESET:
        err = ECONNRESET;
        break;
    case WSAEISCONN:
        err = EISCONN;
        break;
    case WSAENOTCONN:
        err = ENOTCONN;
        break;
    case WSAETIMEDOUT:
        err = ETIMEDOUT;
        break;
    case WSAECONNREFUSED:
        err = ECONNREFUSED;
        break;
    case WSAEHOSTUNREACH:
        err = EHOSTUNREACH;
        break;
    case WSAEDESTADDRREQ:
        err = EDESTADDRREQ;
        break;
    case WSAEMSGSIZE:
        err = EMSGSIZE;
        break;
    case WSAEPROTOTYPE:
        err = EPROTOTYPE;
        break;
    case WSAENOPROTOOPT:
        err = ENOPROTOOPT;
        break;
    case WSAEADDRNOTAVAIL:
        err = EADDRNOTAVAIL;
        break;
    case WSAENOBUFS:
        err = ENOBUFS;
        break;
    // TODO
    case WSAESTALE:
    case WSAEDISCON:
    case WSA_IO_INCOMPLETE:
    case WSA_IO_PENDING:
    case WSAEALREADY:
    case WSAESHUTDOWN:
    case WSAETOOMANYREFS:
    case WSAELOOP:
    case WSAEHOSTDOWN:
    case WSAEPROCLIM:
    case WSAEUSERS:
    case WSAEDQUOT:
    case WSAEREMOTE:
    case WSASYSNOTREADY:
    case WSANOTINITIALISED:
    case WSAENOMORE:
    case WSAEINVALIDPROCTABLE:
    case WSAEINVALIDPROVIDER:
    case WSAEPROVIDERFAILEDINIT:
    case WSASYSCALLFAILURE:
    case WSASERVICE_NOT_FOUND:
    case WSATYPE_NOT_FOUND:
    case WSA_E_NO_MORE:
    case WSAEREFUSED:
    case WSAHOST_NOT_FOUND:
    case WSATRY_AGAIN:
    case WSANO_RECOVERY:
    case WSANO_DATA:
    case WSA_QOS_RECEIVERS:
    case WSA_QOS_SENDERS:
    case WSA_QOS_NO_SENDERS:
    case WSA_QOS_NO_RECEIVERS:
    case WSA_QOS_REQUEST_CONFIRMED:
    case WSA_QOS_ADMISSION_FAILURE:
    case WSA_QOS_POLICY_FAILURE:
    case WSA_QOS_BAD_STYLE:
    case WSA_QOS_BAD_OBJECT:
    case WSA_QOS_TRAFFIC_CTRL_ERROR:
    case WSA_QOS_GENERIC_ERROR:
    case WSA_QOS_ESERVICETYPE:
    case WSA_QOS_EFLOWSPEC:
    case WSA_QOS_EPROVSPECBUF:
    case WSA_QOS_EFILTERSTYLE:
    case WSA_QOS_EFILTERTYPE:
    case WSA_QOS_EFILTERCOUNT:
    case WSA_QOS_EOBJLENGTH:
    case WSA_QOS_EFLOWCOUNT:
    case WSA_QOS_EUNKOWNPSOBJ:
    case WSA_QOS_EPOLICYOBJ:
    case WSA_QOS_EFLOWDESC:
    case WSA_QOS_EPSFLOWSPEC:
    case WSA_QOS_EPSFILTERSPEC:
    case WSA_QOS_ESDMODEOBJ:
    case WSA_QOS_ESHAPERATEOBJ:
    case WSA_QOS_RESERVED_PETYPE:
        break;
    }
    errno = err;
}

SPICE_CONSTRUCTOR_FUNC(socket_win32_init)
{
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

int socket_newpair(int type, int protocol, int sv[2])
{
    struct sockaddr_in sa, sa2;
    socklen_t addrlen;
    SOCKET s, pairs[2];

    if (!sv) {
        return -1;
    }

    /* create a listener */
    s = socket(AF_INET, type, 0);
    if (s == INVALID_SOCKET) {
        return -1;
    }

    pairs[1] = INVALID_SOCKET;

    pairs[0] = socket(AF_INET, type, 0);
    if (pairs[0] == INVALID_SOCKET) {
        goto cleanup;
    }

    /* bind to a random port */
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(s, (struct sockaddr*) &sa, sizeof(sa)) < 0) {
        goto cleanup;
    }
    if (listen(s, 1) < 0) {
        goto cleanup;
    }

    /* connect to kernel choosen port */
    addrlen = sizeof(sa);
    if (getsockname(s, (struct sockaddr*) &sa, &addrlen) < 0) {
        goto cleanup;
    }
    if (connect(pairs[0], (struct sockaddr*) &sa, sizeof(sa)) < 0) {
        goto cleanup;
    }
    addrlen = sizeof(sa2);
    pairs[1] = accept(s, (struct sockaddr*) &sa2, &addrlen);
    if (pairs[1] == INVALID_SOCKET) {
        goto cleanup;
    }

    /* check proper connection */
    addrlen = sizeof(sa);
    if (getsockname(pairs[0], (struct sockaddr*) &sa, &addrlen) < 0) {
        goto cleanup;
    }
    addrlen = sizeof(sa2);
    if (getpeername(pairs[1], (struct sockaddr*) &sa2, &addrlen) < 0) {
        goto cleanup;
    }
    if (sa.sin_family != sa2.sin_family || sa.sin_port != sa2.sin_port
        || sa.sin_addr.s_addr != sa2.sin_addr.s_addr) {
        goto cleanup;
    }

    closesocket(s);
    sv[0] = pairs[0];
    sv[1] = pairs[1];
    return 0;

cleanup:
    socket_win32_set_errno();
    closesocket(s);
    closesocket(pairs[0]);
    closesocket(pairs[1]);
    return -1;
}
#endif
