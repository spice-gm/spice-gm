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

#define WEBSOCKET_MAX_HEADER_SIZE (1 + 9 + 4)

struct RedStream;

typedef struct {
    int type;
    int masked;
    uint8_t header[WEBSOCKET_MAX_HEADER_SIZE];
    int header_pos;
    int frame_ready:1;
    uint8_t mask[4];
    uint64_t relayed;
    uint64_t expected_len;
} websocket_frame_t;

typedef ssize_t (*websocket_read_cb_t)(void *opaque, void *buf, size_t nbyte);
typedef ssize_t (*websocket_write_cb_t)(void *opaque, const void *buf, size_t nbyte);
typedef ssize_t (*websocket_writev_cb_t)(void *opaque, struct iovec *iov, int iovcnt);

typedef struct {
    int closed;

    websocket_frame_t read_frame;
    uint64_t write_remainder;

    struct RedStream *raw_stream;
    websocket_read_cb_t raw_read;
    websocket_write_cb_t raw_write;
    websocket_writev_cb_t raw_writev;
} RedsWebSocket;

RedsWebSocket *websocket_new(char *buf, struct RedStream *s, websocket_read_cb_t read_cb,
                             websocket_write_cb_t write_cb, websocket_writev_cb_t writev_cb);
bool websocket_is_start(char *buf);
void websocket_create_reply(char *buf, char *outbuf);
int websocket_read(RedsWebSocket *ws, uint8_t *buf, size_t len);
int websocket_write(RedsWebSocket *ws, const void *buf, size_t len);
int websocket_writev(RedsWebSocket *ws, const struct iovec *iov, int iovcnt);
