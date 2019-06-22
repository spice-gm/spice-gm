/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2015 Jeremy White

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
#define _GNU_SOURCE
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <sys/types.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <glib.h>

#include <common/log.h>
#include <common/mem.h>

#include "sys-socket.h"
#include "websocket.h"

#ifdef _WIN32
#include <shlwapi.h>
#define strcasestr(haystack, needle) StrStrIA(haystack, needle)
#endif

/* Constants / masks all from RFC 6455 */

#define FIN_FLAG        0x80
#define TYPE_MASK       0x0F

#define CONTINUATION_FRAME  0x0
#define TEXT_FRAME      0x1
#define BINARY_FRAME    0x2
#define CLOSE_FRAME     0x8
#define PING_FRAME      0x9
#define PONG_FRAME      0xA

#define LENGTH_MASK     0x7F
#define LENGTH_16BIT    0x7E
#define LENGTH_64BIT    0x7F

#define MASK_FLAG       0x80

#define WEBSOCKET_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static void websocket_ack_close(void *opaque, websocket_write_cb_t write_cb);

/* Perform a case insensitive search for needle in haystack.
   If found, return a pointer to the byte after the end of needle.
   Otherwise, return NULL */
static const char *find_str(const char *haystack, const char *needle)
{
    const char *s = strcasestr(haystack, needle);

    if (s) {
        return s + strlen(needle);
    }
    return NULL;
}

/* Extract WebSocket style length. Returns 0 if not enough data present,
   Always updates the output 'used' variable to the number of bytes
   required to extract the length; useful for tracking where the
   mask will be.
*/
static uint64_t extract_length(const uint8_t *buf, int *used)
{
    int i;
    uint64_t outlen = (*buf++) & LENGTH_MASK;

    (*used)++;

    switch (outlen) {
    case LENGTH_64BIT:
        *used += 8;
        outlen = 0;
        for (i = 56; i >= 0; i -= 8) {
            outlen |= (*buf++) << i;
        }
        break;

    case LENGTH_16BIT:
        *used += 2;
        outlen = ((*buf) << 8) | *(buf + 1);
        break;

    default:
        break;
    }
    return outlen;
}

static int frame_bytes_needed(websocket_frame_t *frame)
{
    int needed = 2;
    if (frame->header_pos < needed) {
        return needed - frame->header_pos;
    }

    switch (frame->header[1] & LENGTH_MASK) {
    case LENGTH_64BIT:
        needed += 8;
        break;
    case LENGTH_16BIT:
        needed += 2;
        break;
    }

    if (frame->header[1] & MASK_FLAG) {
        needed += 4;
    }

    return needed - frame->header_pos;
}

/*
* Generate WebSocket style response key, based on the
*  original key sent to us
* If non null, caller must free returned key string.
*/
static char *generate_reply_key(char *buf)
{
    GChecksum *checksum;
    char *b64 = NULL;
    uint8_t *sha1;
    size_t sha1_size;
    const char *key;
    const char *p;
    char *k;

    key = find_str(buf, "\nSec-WebSocket-Key:");
    if (key) {
        p = strchr(key, '\r');
        if (p) {
            k = g_strndup(key, p - key);
            k = g_strstrip(k);
            checksum = g_checksum_new(G_CHECKSUM_SHA1);
            g_checksum_update(checksum, (uint8_t *) k, strlen(k));
            g_checksum_update(checksum, (uint8_t *) WEBSOCKET_GUID, strlen(WEBSOCKET_GUID));
            g_free(k);

            sha1_size = g_checksum_type_get_length(G_CHECKSUM_SHA1);
            sha1 = g_malloc(sha1_size);

            g_checksum_get_digest(checksum, sha1, &sha1_size);

            b64 = g_base64_encode(sha1, sha1_size);

            g_checksum_free(checksum);
            g_free(sha1);
        }
    }

    return b64;
}


static void websocket_clear_frame(websocket_frame_t *frame)
{
    memset(frame, 0, sizeof(*frame));
}

/* Extract a frame header of data from a set of data transmitted by
    a WebSocket client. */
static void websocket_get_frame_header(websocket_frame_t *frame)
{
    int fin;
    int used = 0;

    if (frame_bytes_needed(frame) > 0) {
        return;
    }

    fin = frame->header[0] & FIN_FLAG;
    frame->type = frame->header[0] & TYPE_MASK;
    used++;

    frame->masked = frame->header[1] & MASK_FLAG;

    /* This is a Spice specific optimization.  We don't really
       care about assembling frames fully, so we treat
       a frame in process as a finished frame and pass it along. */
    if (!fin && frame->type == CONTINUATION_FRAME) {
        frame->type = BINARY_FRAME;
    }

    frame->expected_len = extract_length(frame->header + used, &used);

    if (frame->masked) {
        memcpy(frame->mask, frame->header + used, 4);
    }

    frame->relayed = 0;
    frame->frame_ready = 1;
}

static int relay_data(uint8_t* buf, size_t size, websocket_frame_t *frame)
{
    int i;
    int n = MIN(size, frame->expected_len - frame->relayed);

    if (frame->masked) {
        for (i = 0; i < n; i++, frame->relayed++) {
            *buf++ ^= frame->mask[frame->relayed % 4];
        }
    }

    return n;
}

int websocket_read(RedsWebSocket *ws, uint8_t *buf, size_t size)
{
    int n = 0;
    int rc;
    websocket_frame_t *frame = &ws->read_frame;
    void *opaque = ws->raw_stream;
    websocket_read_cb_t read_cb = (websocket_read_cb_t) ws->raw_read;
    websocket_write_cb_t write_cb = (websocket_write_cb_t) ws->raw_write;

    if (ws->closed) {
        return 0;
    }

    while (size > 0) {
        // make sure we have a proper frame ready
        if (!frame->frame_ready) {
            rc = read_cb(ws->raw_stream, frame->header + frame->header_pos, frame_bytes_needed(frame));
            if (rc <= 0) {
                goto read_error;
            }
            frame->header_pos += rc;

            websocket_get_frame_header(frame);
        } else if (frame->type == CLOSE_FRAME) {
            websocket_ack_close(opaque, write_cb);
            websocket_clear_frame(frame);
            ws->closed = true;
            return 0;
        } else if (frame->type == BINARY_FRAME) {
            rc = read_cb(opaque, buf, MIN(size, frame->expected_len - frame->relayed));
            if (rc <= 0) {
                goto read_error;
            }

            rc = relay_data(buf, rc, frame);
            n += rc;
            buf += rc;
            size -= rc;
            if (frame->relayed >= frame->expected_len) {
                websocket_clear_frame(frame);
            }
        } else {
            /* TODO - We don't handle PING at this point */
            spice_warning("Unexpected WebSocket frame.type %d.  Failure now likely.", frame->type);
            websocket_clear_frame(frame);
            continue;
        }
    }

    return n;

read_error:
    if (n > 0 && rc == -1 && (errno == EINTR || errno == EAGAIN)) {
        return n;
    }
    if (rc == 0) {
        ws->closed = true;
    }
    return rc;
}

static int fill_header(uint8_t *header, uint64_t len)
{
    int used = 0;
    int i;

    header[0] = FIN_FLAG | BINARY_FRAME;
    used++;

    header[1] = 0;
    used++;
    if (len > 65535) {
        header[1] |= LENGTH_64BIT;
        for (i = 9; i >= 2; i--) {
            header[i] = len & 0xFF;
            len >>= 8;
        }
        used += 8;
    } else if (len >= LENGTH_16BIT) {
        header[1] |= LENGTH_16BIT;
        header[2] = len >> 8;
        header[3] = len & 0xFF;
        used += 2;
    } else {
        header[1] |= len;
    }

    return used;
}

static void constrain_iov(struct iovec *iov, int iovcnt,
                          struct iovec **iov_out, int *iov_out_cnt,
                          uint64_t maxlen)
{
    int i;

    for (i = 0; i < iovcnt && maxlen > 0; i++) {
        if (iov[i].iov_len > maxlen) {
            /* TODO - This code has never triggered afaik... */
            *iov_out_cnt = ++i;
            *iov_out = g_memdup(iov, i * sizeof (*iov));
            (*iov_out)[i-1].iov_len = maxlen;
            return;
        }
        maxlen -= iov[i].iov_len;
    }

    /* we must trim the iov in case maxlen initially matches some chunks
     * For instance if initially we had 2 chunks 256 and 128 bytes respectively
     * and a maxlen of 256 we should just return the first chunk */
    *iov_out_cnt = i;
    *iov_out = iov;
}


/* Write a WebSocket frame with the enclosed data out. */
int websocket_writev(RedsWebSocket *ws, const struct iovec *iov, int iovcnt)
{
    uint8_t header[WEBSOCKET_MAX_HEADER_SIZE];
    uint64_t len;
    int rc = -1;
    struct iovec *iov_out;
    int iov_out_cnt;
    int i;
    int header_len;
    void *opaque = ws->raw_stream;
    websocket_writev_cb_t writev_cb = (websocket_writev_cb_t) ws->raw_writev;
    uint64_t *remainder = &ws->write_remainder;

    if (ws->closed) {
        errno = EPIPE;
        return -1;
    }
    if (*remainder > 0) {
        constrain_iov((struct iovec *) iov, iovcnt, &iov_out, &iov_out_cnt, *remainder);
        rc = writev_cb(opaque, iov_out, iov_out_cnt);
        if (iov_out != iov) {
            g_free(iov_out);
        }
        if (rc <= 0) {
            return rc;
        }
        *remainder -= rc;
        return rc;
    }

    iov_out_cnt = iovcnt + 1;
    iov_out = g_malloc(iov_out_cnt * sizeof(*iov_out));

    for (i = 0, len = 0; i < iovcnt; i++) {
        len += iov[i].iov_len;
        iov_out[i + 1] = iov[i];
    }

    memset(header, 0, sizeof(header));
    header_len = fill_header(header, len);
    iov_out[0].iov_len = header_len;
    iov_out[0].iov_base = header;
    rc = writev_cb(opaque, iov_out, iov_out_cnt);
    g_free(iov_out);
    if (rc <= 0) {
        return rc;
    }
    rc -= header_len;

    spice_assert(rc >= 0);

    /* Key point:  if we did not write out all the data, remember how
       much more data the client is expecting, and write that data without
       a header of any kind the next time around */
    *remainder = len - rc;

    return rc;
}

int websocket_write(RedsWebSocket *ws, const void *buf, size_t len)
{
    uint8_t header[WEBSOCKET_MAX_HEADER_SIZE];
    int rc;
    int header_len;
    void *opaque = ws->raw_stream;
    websocket_write_cb_t write_cb = (websocket_write_cb_t) ws->raw_write;
    uint64_t *remainder = &ws->write_remainder;

    if (ws->closed) {
        errno = EPIPE;
        return -1;
    }

    if (*remainder == 0) {
        header_len = fill_header(header, len);
        rc = write_cb(opaque, header, header_len);
        if (rc <= 0) {
            return rc;
        }
        if (rc != header_len) {
            /* TODO - In theory, we can handle this case.  In practice,
                      it does not occur, and does not seem to be worth
                      the code complexity */
            errno = EPIPE;
            return -1;
        }
    } else {
        len = MIN(*remainder, len);
    }

    rc = write_cb(opaque, buf, len);
    if (rc <= 0) {
        *remainder = len;
    } else {
        *remainder = len - rc;
    }
    return rc;
}

static void websocket_ack_close(void *opaque, websocket_write_cb_t write_cb)
{
    unsigned char header[2];

    header[0] = FIN_FLAG | CLOSE_FRAME;
    header[1] = 0;

    write_cb(opaque, header, sizeof(header));
}

bool websocket_is_start(char *buf)
{
    if (strncmp(buf, "GET ", 4) == 0 &&
            // TODO strip, do not assume a single space
            find_str(buf, "\nSec-WebSocket-Protocol: binary") &&
            find_str(buf, "\nSec-WebSocket-Key:") &&
            g_str_has_suffix(buf, "\r\n\r\n")) {
        return true;
    }

    return false;
}

void websocket_create_reply(char *buf, char *outbuf)
{
    char *key;

    key = generate_reply_key(buf);
    sprintf(outbuf, "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: %s\r\n"
                    "Sec-WebSocket-Protocol: binary\r\n\r\n", key);
    g_free(key);
}
