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

#include "internal.h"
#include "decoders.h"

static const AVClass decoder_context_class = {
    .class_name = "decoder",
    .item_name  = av_default_item_name,
};

struct decoder_ctx *decoder_alloc(void)
{
    struct decoder_ctx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->class = &decoder_context_class;
    ctx->avctx = avcodec_alloc_context3(NULL);
    if (!ctx->avctx) {
        av_freep(&ctx);
        return NULL;
    }
    return ctx;
}

int decoder_init(struct decoder_ctx *ctx,
                 const struct decoder *dec,
                 const AVStream *stream,
                 struct decoding_ctx *decoding_ctx)
{
    int ret;

    TRACE(ctx, "try to initialize private decoder");

    if (dec->priv_data_size) {
        ctx->priv_data = av_mallocz(dec->priv_data_size);
        if (!ctx->priv_data)
            return AVERROR(ENOMEM);
    }

    // We need to copy the stream information because the stream (and its codec
    // context) can be destroyed any time after the decoder_init() returns
    avcodec_copy_context(ctx->avctx, stream->codec);

    ret = dec->init(ctx);
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

int decoder_push_packet(struct decoder_ctx *ctx, const AVPacket *pkt)
{
    return ctx->dec->push_packet(ctx, pkt);
}

void decoder_flush(struct decoder_ctx *ctx)
{
    TRACE(ctx, "flush");
    ctx->dec->flush(ctx);
}

void decoder_free(struct decoder_ctx **ctxp)
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
