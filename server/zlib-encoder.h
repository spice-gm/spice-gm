/*
   Copyright (C) 2009 Red Hat, Inc.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in
         the documentation and/or other materials provided with the
         distribution.
       * Neither the name of the copyright holder nor the names of its
         contributors may be used to endorse or promote products derived
         from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef ZLIB_ENCODER_H_
#define ZLIB_ENCODER_H_

#include <inttypes.h>

SPICE_BEGIN_DECLS

typedef struct ZlibEncoder ZlibEncoder;
typedef struct ZlibEncoderUsrContext ZlibEncoderUsrContext;

struct ZlibEncoderUsrContext {
    int (*more_space)(ZlibEncoderUsrContext *usr, uint8_t **io_ptr);
    int (*more_input)(ZlibEncoderUsrContext *usr, uint8_t **input);
};

ZlibEncoder* zlib_encoder_create(ZlibEncoderUsrContext *usr, int level);
void zlib_encoder_destroy(ZlibEncoder *encoder);

/* returns the total size of the encoded data */
int zlib_encode(ZlibEncoder *zlib, int level, int input_size,
                uint8_t *io_ptr, unsigned int num_io_bytes);

SPICE_END_DECLS

#endif /* ZLIB_ENCODER_H_ */
