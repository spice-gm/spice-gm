/*
 *  Copyright (C) 2015 Jeremy White
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef WEBSOCKET_H_
#define WEBSOCKET_H_

#include <stdint.h>

#include "sys-socket.h"

typedef ssize_t (*websocket_read_cb_t)(void *opaque, void *buf, size_t nbyte);
typedef ssize_t (*websocket_write_cb_t)(void *opaque, const void *buf, size_t nbyte);
typedef ssize_t (*websocket_writev_cb_t)(void *opaque, struct iovec *iov, int iovcnt);

typedef struct RedsWebSocket RedsWebSocket;

enum {
    WEBSOCKET_TEXT = 1,
    WEBSOCKET_BINARY= 2,
    WEBSOCKET_FINAL = 0x80,
    WEBSOCKET_TEXT_FINAL = WEBSOCKET_TEXT | WEBSOCKET_FINAL,
    WEBSOCKET_BINARY_FINAL = WEBSOCKET_BINARY | WEBSOCKET_FINAL,
};

RedsWebSocket *websocket_new(const void *buf, size_t len, void *stream, websocket_read_cb_t read_cb,
                             websocket_write_cb_t write_cb, websocket_writev_cb_t writev_cb);
void websocket_free(RedsWebSocket *ws);

/**
 * Read data from websocket.
 * Can return 0 in case client sent an empty text/binary data, check
 * flags to detect this.
 */
int websocket_read(RedsWebSocket *ws, uint8_t *buf, size_t len, unsigned *flags);
int websocket_write(RedsWebSocket *ws, const void *buf, size_t len, unsigned flags);
int websocket_writev(RedsWebSocket *ws, const struct iovec *iov, int iovcnt, unsigned flags);

#endif
