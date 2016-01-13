/*
 * This file is part of sxplayer.
 *
 * Copyright (c) 2015 Stupeflix
 *
 * sxplayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * sxplayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with sxplayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef SXPLAYER_DECODERS_H
#define SXPLAYER_DECODERS_H

#include <libavcodec/avcodec.h>

struct decoder_ctx {
    const AVClass *class;
    AVCodecContext *avctx;
    struct async_decoder *adec;

    const struct decoder *dec_default;
    const struct decoder *dec_fallback;
    const AVCodecContext *avctx_orig;
    const struct decoder *dec; // point to dec_default or dec_fallback
    void *priv_data;
};

struct decoder {
    int (*init)(struct decoder_ctx *ctx, void *opaque);
    void (*uninit)(struct decoder_ctx *ctx);
    int (*push_packet)(struct decoder_ctx *ctx, const AVPacket *pkt);
    void (*flush)(struct decoder_ctx *ctx);
    int priv_data_size;
    const char *supported_codecs;
};

struct decoder_ctx *decoder_create(const struct decoder *dec_default,
                                   const struct decoder *dec_fallback,
                                   const AVCodecContext *avctx);

int decoder_init(struct decoder_ctx *ctx, void *priv);
int decoder_push_packet(struct decoder_ctx *ctx, const AVPacket *pkt);
void decoder_flush(struct decoder_ctx *ctx);
void decoder_uninit(struct decoder_ctx *ctx);
void decoder_free(struct decoder_ctx **ctxp);

#endif
