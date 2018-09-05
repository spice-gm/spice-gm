/*
   Copyright (C) 2009, 2013 Red Hat, Inc.

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

#ifndef RED_STREAM_H_
#define RED_STREAM_H_

#include <openssl/ssl.h>

#include "spice.h"
#include "red-common.h"

typedef void (*AsyncReadDone)(void *opaque);
typedef void (*AsyncReadError)(void *opaque, int err);

typedef struct RedStream RedStream;
typedef struct RedStreamPrivate RedStreamPrivate;

struct RedStream {
    int socket;
    SpiceWatch *watch;

    RedStreamPrivate *priv;
};

typedef enum {
    RED_STREAM_SSL_STATUS_OK,
    RED_STREAM_SSL_STATUS_ERROR,
    RED_STREAM_SSL_STATUS_WAIT_FOR_READ,
    RED_STREAM_SSL_STATUS_WAIT_FOR_WRITE
} RedStreamSslStatus;

/* any thread */
ssize_t red_stream_read(RedStream *s, void *buf, size_t nbyte);
void red_stream_async_read(RedStream *stream, uint8_t *data, size_t size,
                           AsyncReadDone read_done_cb, void *opaque);
void red_stream_set_async_error_handler(RedStream *stream,
                                        AsyncReadError error_handler);
ssize_t red_stream_write(RedStream *s, const void *buf, size_t nbyte);
ssize_t red_stream_writev(RedStream *s, const struct iovec *iov, int iovcnt);
bool red_stream_write_all(RedStream *stream, const void *in_buf, size_t n);
void red_stream_free(RedStream *s);

void red_stream_push_channel_event(RedStream *s, int event);
void red_stream_remove_watch(RedStream* s);
void red_stream_set_channel(RedStream *stream, int connection_id,
                            int channel_type, int channel_id);
RedStream *red_stream_new(RedsState *reds, int socket);
void red_stream_set_core_interface(RedStream *stream, SpiceCoreInterfaceInternal *core);
bool red_stream_is_ssl(RedStream *stream);
RedStreamSslStatus red_stream_ssl_accept(RedStream *stream);
RedStreamSslStatus red_stream_enable_ssl(RedStream *stream, SSL_CTX *ctx);
int red_stream_get_family(const RedStream *stream);
bool red_stream_is_plain_unix(const RedStream *stream);
bool red_stream_set_no_delay(RedStream *stream, bool no_delay);
int red_stream_get_no_delay(RedStream *stream);
int red_stream_send_msgfd(RedStream *stream, int fd);

/**
 * Set auto flush flag.
 * If set, stream will send data to the underlying socket as
 * soon as data are written. This is the default.
 * If not set, you should call red_stream_flush to force
 * data to be sent. Failing to call red_stream_flush on a
 * manual flush stream could lead to latency.
 * Disabling auto flush can fail while enabling cannot.
 *
 * Returns true on success or false on failure.
 */
bool red_stream_set_auto_flush(RedStream *stream, bool auto_flush);

/**
 * Flush data to the underlying socket.
 * Calling this function on a stream with auto flush set has
 * no result.
 */
void red_stream_flush(RedStream *stream);

typedef enum {
    RED_SASL_ERROR_OK,
    RED_SASL_ERROR_GENERIC,
    RED_SASL_ERROR_INVALID_DATA,
    RED_SASL_ERROR_AUTH_FAILED
} RedSaslError;

typedef void (*RedSaslResult)(void *opaque, RedSaslError err);
bool red_sasl_start_auth(RedStream *stream, RedSaslResult result_cb, void *opaque);

#endif /* RED_STREAM_H_ */
