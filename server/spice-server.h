/*
 *  Copyright (C) 2009-2014 Red Hat, Inc.
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

#ifndef SPICE_SERVER_H_
#define SPICE_SERVER_H_

#if !defined(SPICE_H_INSIDE) && !defined(SPICE_SERVER_INTERNAL)
#error "Only spice.h can be included directly."
#endif

#include "spice-core.h"

SPICE_BEGIN_DECLS

/* Don't use features incompatible with a specific spice
   version, so that migration to/from that version works. */
typedef enum {
    SPICE_COMPAT_VERSION_0_4 = 0,
    SPICE_COMPAT_VERSION_0_6 = 1,
} spice_compat_version_t;

#define SPICE_COMPAT_VERSION_CURRENT SPICE_COMPAT_VERSION_0_6

spice_compat_version_t spice_get_current_compat_version(void);

typedef struct RedsState SpiceServer;
SpiceServer *spice_server_new(void);
int spice_server_init(SpiceServer *s, SpiceCoreInterface *core);
void spice_server_destroy(SpiceServer *s);

#define SPICE_ADDR_FLAG_IPV4_ONLY (1 << 0)
#define SPICE_ADDR_FLAG_IPV6_ONLY (1 << 1)
#define SPICE_ADDR_FLAG_UNIX_ONLY (1 << 2)

int spice_server_set_compat_version(SpiceServer *s,
                                    spice_compat_version_t version);
int spice_server_set_port(SpiceServer *s, int port);
void spice_server_set_addr(SpiceServer *s, const char *addr, int flags);
int spice_server_set_listen_socket_fd(SpiceServer *s, int listen_fd) SPICE_GNUC_DEPRECATED;
int spice_server_set_exit_on_disconnect(SpiceServer *s, int flag);
int spice_server_set_noauth(SpiceServer *s);
int spice_server_set_sasl(SpiceServer *s, int enabled);
int spice_server_set_sasl_appname(SpiceServer *s, const char *appname);
int spice_server_set_ticket(SpiceServer *s, const char *passwd, int lifetime,
                            int fail_if_connected, int disconnect_if_connected);
void spice_server_set_ticket_handler(SpiceServer *reds, const char *ticket_handler);
int spice_server_set_tls(SpiceServer *s, int port,
                         const char *ca_cert_file, const char *certs_file,
                         const char *private_key_file, const char *key_passwd,
                         const char *dh_key_file, const char *ciphersuite);

int spice_server_add_client(SpiceServer *s, int socket, int skip_auth);
int spice_server_add_ssl_client(SpiceServer *s, int socket, int skip_auth);

int spice_server_add_interface(SpiceServer *s,
                               SpiceBaseInstance *sin);
/**
 * Remove an interface from SpiceServer.
 * SpiceServer won't be using the interface anymore, so it can
 * be freed or reused.
 */
int spice_server_remove_interface(SpiceBaseInstance *sin);

/* XXX This definition is here only to make glib generation
 * of enumerators possible
 */
#if 0
typedef enum {
    SPICE_IMAGE_COMPRESSION_INVALID  = 0,
    SPICE_IMAGE_COMPRESSION_OFF      = 1,
    SPICE_IMAGE_COMPRESSION_AUTO_GLZ = 2,
    SPICE_IMAGE_COMPRESSION_AUTO_LZ  = 3,
    SPICE_IMAGE_COMPRESSION_QUIC     = 4,
    SPICE_IMAGE_COMPRESSION_GLZ      = 5,
    SPICE_IMAGE_COMPRESSION_LZ       = 6,
    SPICE_IMAGE_COMPRESSION_LZ4      = 7,
} spice_image_compression_t;
#endif

// Needed for backward API compatibility
typedef SpiceImageCompression spice_image_compression_t;
#define SPICE_IMAGE_COMPRESS_INVALID SPICE_IMAGE_COMPRESSION_INVALID
#define SPICE_IMAGE_COMPRESS_OFF SPICE_IMAGE_COMPRESSION_OFF
#define SPICE_IMAGE_COMPRESS_AUTO_GLZ SPICE_IMAGE_COMPRESSION_AUTO_GLZ
#define SPICE_IMAGE_COMPRESS_AUTO_LZ SPICE_IMAGE_COMPRESSION_AUTO_LZ
#define SPICE_IMAGE_COMPRESS_QUIC SPICE_IMAGE_COMPRESSION_QUIC
#define SPICE_IMAGE_COMPRESS_GLZ SPICE_IMAGE_COMPRESSION_GLZ
#define SPICE_IMAGE_COMPRESS_LZ SPICE_IMAGE_COMPRESSION_LZ
#define SPICE_IMAGE_COMPRESS_LZ4 SPICE_IMAGE_COMPRESSION_LZ4

int spice_server_set_image_compression(SpiceServer *s,
                                       SpiceImageCompression comp);
SpiceImageCompression spice_server_get_image_compression(SpiceServer *s);

typedef enum {
    SPICE_WAN_COMPRESSION_INVALID,
    SPICE_WAN_COMPRESSION_AUTO,
    SPICE_WAN_COMPRESSION_ALWAYS,
    SPICE_WAN_COMPRESSION_NEVER,
} spice_wan_compression_t;

int spice_server_set_jpeg_compression(SpiceServer *s, spice_wan_compression_t comp);
int spice_server_set_zlib_glz_compression(SpiceServer *s, spice_wan_compression_t comp);

#define SPICE_CHANNEL_SECURITY_NONE (1 << 0)
#define SPICE_CHANNEL_SECURITY_SSL (1 << 1)

int spice_server_set_channel_security(SpiceServer *s, const char *channel, int security);

int spice_server_add_renderer(SpiceServer *s, const char *name) SPICE_GNUC_DEPRECATED;

enum {
    SPICE_STREAM_VIDEO_INVALID,
    SPICE_STREAM_VIDEO_OFF,
    SPICE_STREAM_VIDEO_ALL,
    SPICE_STREAM_VIDEO_FILTER
};

int spice_server_set_streaming_video(SpiceServer *s, int value);

enum {
    SPICE_STREAMING_INVALID,
    SPICE_STREAMING_SPICE,
    SPICE_STREAMING_GSTREAMER
};

int spice_server_set_video_codecs(SpiceServer *s, const char* video_codecs);

/**
 * Returns a newly allocated string describing video encoders/codecs
 * currently allowed in @s Spice server. The string returned by
 * this function must be released with spice_server_free_video_codecs.
 *
 * @s: the Spice server to query
 * @return the string describing the video encoders/codecs currently enabled
 */
const char *spice_server_get_video_codecs(SpiceServer *s);

/**
 * Releases the memory of the video-codec string returned by
 * spice_server_get_video_codecs.
 */
void spice_server_free_video_codecs(SpiceServer *s, const char *video_codecs);
int spice_server_set_playback_compression(SpiceServer *s, int enable);
int spice_server_set_agent_mouse(SpiceServer *s, int enable);
int spice_server_set_agent_copypaste(SpiceServer *s, int enable);
int spice_server_set_agent_file_xfer(SpiceServer *s, int enable);

int spice_server_get_sock_info(SpiceServer *s, struct sockaddr *sa, socklen_t *salen) SPICE_GNUC_DEPRECATED;
int spice_server_get_peer_info(SpiceServer *s, struct sockaddr *sa, socklen_t *salen) SPICE_GNUC_DEPRECATED;

int spice_server_is_server_mouse(SpiceServer *s);

void spice_server_set_name(SpiceServer *s, const char *name);
void spice_server_set_uuid(SpiceServer *s, const uint8_t uuid[16]);

void spice_server_vm_start(SpiceServer *s);
void spice_server_vm_stop(SpiceServer *s);

int spice_server_get_num_clients(SpiceServer *s) SPICE_GNUC_DEPRECATED;

SPICE_END_DECLS

#endif /* SPICE_SERVER_H_ */
