/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2015 Red Hat, Inc.

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

#ifndef IMAGE_ENCODERS_H_
#define IMAGE_ENCODERS_H_

#include <setjmp.h>
#include <pthread.h>
#include <common/quic.h>
#include <common/lz.h>
#include <common/ring.h>

#include "stat.h"
#include "red-parse-qxl.h"
#include "glz-encoder.h"
#include "jpeg-encoder.h"
#ifdef USE_LZ4
#include "lz4-encoder.h"
#endif
#include "zlib-encoder.h"

SPICE_BEGIN_DECLS

struct RedClient;

typedef struct RedCompressBuf RedCompressBuf;
typedef struct ImageEncoders ImageEncoders;
typedef struct ImageEncoderSharedData ImageEncoderSharedData;
typedef struct GlzSharedDictionary GlzSharedDictionary;
typedef struct GlzImageRetention GlzImageRetention;

void image_encoder_shared_init(ImageEncoderSharedData *shared_data);
void image_encoder_shared_stat_reset(ImageEncoderSharedData *shared_data);
void image_encoder_shared_stat_print(const ImageEncoderSharedData *shared_data);

void image_encoders_init(ImageEncoders *enc, ImageEncoderSharedData *shared_data);
void image_encoders_free(ImageEncoders *enc);
int image_encoders_free_some_independent_glz_drawables(ImageEncoders *enc);
void image_encoders_free_glz_drawables(ImageEncoders *enc);
void image_encoders_free_glz_drawables_to_free(ImageEncoders* enc);
gboolean image_encoders_glz_create(ImageEncoders *enc, uint8_t id);
void image_encoders_glz_get_restore_data(ImageEncoders *enc,
                                         uint8_t *out_id, GlzEncDictRestoreData *out_data);
gboolean image_encoders_glz_encode_lock(ImageEncoders *enc);
void image_encoders_glz_encode_unlock(ImageEncoders *enc);
void glz_retention_free_drawables(GlzImageRetention *ret);
void glz_retention_detach_drawables(GlzImageRetention *ret);

#define RED_COMPRESS_BUF_SIZE (1024 * 64)
struct RedCompressBuf {
    RedCompressBuf *send_next;

    /* This buffer provide space for compression algorithms.
     * Some algorithms access the buffer as an array of 32 bit words
     * so is defined to make sure is always aligned that way.
     */
    union {
        uint8_t  bytes[RED_COMPRESS_BUF_SIZE];
        uint32_t words[RED_COMPRESS_BUF_SIZE / 4];
    } buf;
};

static inline void compress_buf_free(RedCompressBuf *buf)
{
    g_free(buf);
}

gboolean image_encoders_get_glz_dictionary(ImageEncoders *enc,
                                           struct RedClient *client,
                                           uint8_t id, int window_size);
gboolean image_encoders_restore_glz_dictionary(ImageEncoders *enc,
                                               struct RedClient *client,
                                               uint8_t id,
                                               GlzEncDictRestoreData *restore_data);

typedef struct  {
    RedCompressBuf *bufs_head;
    RedCompressBuf *bufs_tail;
    jmp_buf jmp_env;
    union {
        struct {
            SpiceChunks *chunks;
            int next;
            int stride;
            int reverse;
        } lines_data;
        struct {
            RedCompressBuf* next;
            int size_left;
        } compressed_data; // for encoding data that was already compressed by another method
    } u;
} EncoderData;

typedef struct {
    QuicUsrContext usr;
    EncoderData data;
} QuicData;

typedef struct {
    LzUsrContext usr;
    EncoderData data;
} LzData;

typedef struct {
    JpegEncoderUsrContext usr;
    EncoderData data;
} JpegData;

#ifdef USE_LZ4
typedef struct {
    Lz4EncoderUsrContext usr;
    EncoderData data;
} Lz4Data;
#endif

typedef struct {
    ZlibEncoderUsrContext usr;
    EncoderData data;
} ZlibData;

typedef struct {
    GlzEncoderUsrContext usr;
    EncoderData data;
} GlzData;

struct GlzImageRetention {
    Ring ring;
};

static inline void glz_retention_init(GlzImageRetention *ret)
{
    ring_init(&ret->ring);
}

struct ImageEncoderSharedData {
    uint32_t glz_drawable_count;

    stat_info_t off_stat;
    stat_info_t lz_stat;
    stat_info_t glz_stat;
    stat_info_t quic_stat;
    stat_info_t jpeg_stat;
    stat_info_t zlib_glz_stat;
    stat_info_t jpeg_alpha_stat;
    stat_info_t lz4_stat;
};

struct ImageEncoders {
    ImageEncoderSharedData *shared_data;

    QuicData quic_data;
    QuicContext *quic;

    LzData lz_data;
    LzContext  *lz;

    int jpeg_quality;

    JpegData jpeg_data;
    JpegEncoderContext *jpeg;

#ifdef USE_LZ4
    Lz4Data lz4_data;
    Lz4EncoderContext *lz4;
#endif

    int zlib_level;

    ZlibData zlib_data;
    ZlibEncoder *zlib;

    /* global lz encoding entities */
    GlzSharedDictionary *glz_dict;
    GlzEncoderContext *glz;
    GlzData glz_data;

    Ring glz_drawables;               // all the living lz drawable, ordered by encoding time
    Ring glz_drawables_inst_to_free;               // list of instances to be freed
    pthread_mutex_t glz_drawables_inst_to_free_lock;
};

typedef struct compress_send_data_t {
    RedCompressBuf *comp_buf;
    uint32_t comp_buf_size;
    SpicePalette *lzplt_palette;
    gboolean is_lossy;
} compress_send_data_t;

bool image_encoders_compress_quic(ImageEncoders *enc, SpiceImage *dest,
                                  SpiceBitmap *src, compress_send_data_t* o_comp_data);
bool image_encoders_compress_lz(ImageEncoders *enc, SpiceImage *dest,
                                SpiceBitmap *src, compress_send_data_t* o_comp_data);
bool image_encoders_compress_jpeg(ImageEncoders *enc, SpiceImage *dest,
                                  SpiceBitmap *src, compress_send_data_t* o_comp_data);
#ifdef USE_LZ4
bool image_encoders_compress_lz4(ImageEncoders *enc, SpiceImage *dest,
                                 SpiceBitmap *src, compress_send_data_t* o_comp_data);
#endif
bool image_encoders_compress_glz(ImageEncoders *enc, SpiceImage *dest,
                                 SpiceBitmap *src,
                                 RedDrawable *red_drawable,
                                 GlzImageRetention *glz_retention,
                                 compress_send_data_t* o_comp_data,
                                 gboolean enable_zlib_glz_wrap);

#define RED_RELEASE_BUNCH_SIZE 64

SPICE_END_DECLS

#endif /* IMAGE_ENCODERS_H_ */
