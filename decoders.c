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

struct decoder_ctx *decoder_create(const struct decoder *dec_default,
                                   const struct decoder *dec_fallback,
                                   const AVCodecContext *avctx)
{
    struct decoder_ctx *ctx = av_mallocz(sizeof(*ctx));

    DBG("decoder_create", "create decoder fallback:%s avctx=%p\n",
        dec_fallback ? "yes" : "no", avctx);
    if (!ctx)
        return NULL;
    ctx->dec_default  = dec_default;
    ctx->dec_fallback = dec_fallback;
    ctx->avctx_orig   = avctx;
    return ctx;
}

static void free_context_data(struct decoder_ctx *ctx)
{
    av_freep(&ctx->priv_data);
    avcodec_free_context(&ctx->avctx);
}

static int try_init(struct decoder_ctx *ctx, void *priv)
{
    int ret;
    const struct decoder *dec = ctx->dec;

    DBG("try_init", "try to initialize private decoder\n");

    if (dec->priv_data_size) {
        ctx->priv_data = av_mallocz(dec->priv_data_size);
        if (!ctx->priv_data) {
            ret = AVERROR(ENOMEM);
            goto err;
        }
    }

    ctx->avctx = avcodec_alloc_context3(NULL);
    if (!ctx->avctx) {
        ret = AVERROR(ENOMEM);
        goto err;
    }

    avcodec_copy_context(ctx->avctx, ctx->avctx_orig);
    ret = dec->init(ctx, priv);
    if (ret < 0) {
        decoder_uninit(ctx);
        goto err;
    }

    return 0;

err:
    free_context_data(ctx);
    return ret;
}

int decoder_init(struct decoder_ctx *ctx, void *priv)
{
    int ret;

    DBG("decoder_init", "init %p with priv=%p\n", priv);
    ctx->dec = ctx->dec_default;
    ret = try_init(ctx, priv);
    if (ret < 0 && ctx->dec_fallback) {
        DBG("decoder_init", "try_init() failed, fallback\n");
        if (ret != AVERROR_DECODER_NOT_FOUND)
            fprintf(stderr, "Decoder fallback\n");
        ctx->dec = ctx->dec_fallback;
        ret = try_init(ctx, priv);
    }
    return ret;
}

int decoder_push_packet(struct decoder_ctx *ctx, const AVPacket *pkt)
{
    return ctx->dec->push_packet(ctx, pkt);
}

void decoder_flush(struct decoder_ctx *ctx)
{
    DBG("decoder_flush", "flush %p\n", ctx);
    ctx->dec->flush(ctx);
}

void decoder_uninit(struct decoder_ctx *ctx)
{
    DBG("decoder_uninit", "uninit %p\n", ctx);
    if (ctx->dec && ctx->dec->uninit)
        ctx->dec->uninit(ctx);
}

void decoder_free(struct decoder_ctx **ctxp)
{
    DBG("decoder_free", "free context\n");
    if (*ctxp)
        free_context_data(*ctxp);
    av_freep(ctxp);
}
