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

#include "decoders.h"
#include "internal.h"
#include "log.h"

struct decoder_ctx *sxpi_decoder_alloc(void)
{
    struct decoder_ctx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->avctx = avcodec_alloc_context3(NULL);
    if (!ctx->avctx) {
        av_freep(&ctx);
        return NULL;
    }
    return ctx;
}

int sxpi_decoder_init(void *log_ctx,
                 struct decoder_ctx *ctx,
                 const struct decoder *dec,
                 const AVStream *stream,
                 struct decoding_ctx *decoding_ctx,
                 const struct sxplayer_opts *opts)
{
    int ret;

    ctx->log_ctx = log_ctx;
    ctx->opaque = opts->opaque ? *(void **)opts->opaque : NULL;

    TRACE(ctx, "try to initialize private decoder");

    if (dec->priv_data_size) {
        ctx->priv_data = av_mallocz(dec->priv_data_size);
        if (!ctx->priv_data)
            return AVERROR(ENOMEM);
    }

    // We need to copy the stream information because the stream (and its codec
    // context) can be destroyed any time after the sxpi_decoder_init() returns
    avcodec_parameters_to_context(ctx->avctx, stream->codecpar);

    // The MediaCodec decoder needs pkt_timebase in order to rescale the
    // timestamps that will be forwarded to the output surface
    if (HAVE_MEDIACODEC_HWACCEL && !strcmp(dec->name, "ffmpeg_hw"))
        ctx->avctx->pkt_timebase = stream->time_base;

    ret = dec->init(ctx, opts);
    if (ret < 0) {
        if (dec->uninit)
            dec->uninit(ctx);
        av_freep(&ctx->priv_data);
        return ret;
    }

    ctx->dec = dec;
    ctx->decoding_ctx = decoding_ctx;
    return 0;
}

int sxpi_decoder_push_packet(struct decoder_ctx *ctx, const AVPacket *pkt)
{
    return ctx->dec->push_packet(ctx, pkt);
}

void sxpi_decoder_flush(struct decoder_ctx *ctx)
{
    TRACE(ctx, "flush");
    ctx->dec->flush(ctx);
}

void sxpi_decoder_free(struct decoder_ctx **ctxp)
{
    struct decoder_ctx *ctx = *ctxp;
    if (!ctx)
        return;
    if (ctx->dec && ctx->dec->uninit)
        ctx->dec->uninit(ctx);
    avcodec_free_context(&ctx->avctx);
    av_freep(&ctx->priv_data);
    av_freep(ctxp);
}
