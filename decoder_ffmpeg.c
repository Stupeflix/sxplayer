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

#include <libavcodec/avcodec.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>

#include "decoding.h"
#include "decoders.h"
#include "internal.h"

#if LIBAVCODEC_VERSION_INT >= MEDIACODEC_HWACCEL_VERSION_INT

#include <libavcodec/mediacodec.h>

static enum AVPixelFormat mediacodec_hwaccel_get_format(AVCodecContext *avctx, const enum AVPixelFormat *pix_fmts)
{
    struct decoder_ctx *ctx = avctx->opaque;
    const enum AVPixelFormat *p = NULL;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        AVMediaCodecContext *mediacodec_ctx = NULL;
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            break;

        if (*p != AV_PIX_FMT_MEDIACODEC)
            continue;

        if (ctx->opaque) {
            mediacodec_ctx = av_mediacodec_alloc_context();
            if (!mediacodec_ctx) {
                fprintf(stderr, "Failed to allocate hwaccel ctx\n");
                continue;
            }

            if (av_mediacodec_default_init(avctx, mediacodec_ctx, ctx->opaque) < 0) {
                fprintf(stderr, "Failed to init hwaccel ctx\n");
                continue;
            }
        }
        break;
    }

    return *p;
}

#endif

static int ffdec_init(struct decoder_ctx *ctx, int hw)
{
    int ret;
    AVCodecContext *avctx = ctx->avctx;
    AVCodec *dec = avcodec_find_decoder(avctx->codec_id);

    TRACE(ctx, "initialize context");

    av_opt_set_int(avctx, "refcounted_frames", 1, 0);

    if (hw && avctx->codec_id == AV_CODEC_ID_H264) {
        AVCodec *codec = avcodec_find_decoder_by_name("h264_mediacodec");
        if (!codec)
            return AVERROR_DECODER_NOT_FOUND;

#if LIBAVCODEC_VERSION_INT >= MEDIACODEC_HWACCEL_VERSION_INT
        avctx->opaque = ctx;
        avctx->get_format = mediacodec_hwaccel_get_format;
        avctx->thread_count = 1;
#endif
        dec = codec;
    }

    TRACE(ctx, "codec open");

    ret = avcodec_open2(avctx, dec, NULL);
    if (ret < 0)
        return ret;

    ctx->avctx = avctx;

    return ret;
}

static int ffdec_init_sw(struct decoder_ctx *ctx)
{
    return ffdec_init(ctx, 0);
}

static int ffdec_init_hw(struct decoder_ctx *ctx)
{
    return ffdec_init(ctx, 1);
}

static int decode_packet(struct decoder_ctx *ctx, const AVPacket *pkt, int *got_frame)
{
    int ret;
    int decoded = pkt->size;
    AVCodecContext *avctx = ctx->avctx;
    AVFrame *dec_frame = av_frame_alloc();

    *got_frame = 0;

    TRACE(ctx, "decode packet of size %d", pkt->size);

    if (!dec_frame)
        return AVERROR(ENOMEM);

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        ret = avcodec_decode_video2(avctx, dec_frame, got_frame, pkt);
        break;
    case AVMEDIA_TYPE_AUDIO:
        ret = avcodec_decode_audio4(avctx, dec_frame, got_frame, pkt);
        break;
    default:
        av_assert0(0);
    }

    if (ret < 0) {
        LOG(ctx, ERROR, "Error decoding packet: %s", av_err2str(ret));
        av_frame_free(&dec_frame);
        return ret;
    }

    TRACE(ctx, "decoded %d/%d bytes from packet -> got_frame=%d", ret, pkt->size, *got_frame);
    decoded = FFMIN(ret, pkt->size);

    if (*got_frame) {
        ret = decoding_queue_frame(ctx->decoding_ctx, dec_frame);
        if (ret < 0) {
            TRACE(ctx, "could not queue frame: %s", av_err2str(ret));
            av_frame_free(&dec_frame);
            return ret;
        }
    } else {
        av_frame_free(&dec_frame);
    }
    return decoded;
}


static int ffdec_push_packet(struct decoder_ctx *ctx, const AVPacket *pkt)
{
    int ret;
    const int flush = !pkt->size;
    AVPacket avpkt = *pkt;
    int got_frame;

    TRACE(ctx, "received packet of size %d", pkt->size);
    do {
        ret = decode_packet(ctx, &avpkt, &got_frame);
        if (ret < 0)
            break;
        avpkt.data += ret;
        avpkt.size -= ret;
    } while (avpkt.size > 0 || (flush && got_frame));
    if (ret == 0 && flush && !got_frame)
        return decoding_queue_frame(ctx->decoding_ctx, NULL);
    return ret;
}

static void ffdec_flush(struct decoder_ctx *ctx)
{
    AVCodecContext *avctx = ctx->avctx;
    avcodec_flush_buffers(avctx);
}


static void ffdec_uninit_hw(struct decoder_ctx *ctx)
{
#if LIBAVCODEC_VERSION_INT >= MEDIACODEC_HWACCEL_VERSION_INT
    AVCodecContext *avctx = ctx->avctx;
    const struct AVCodec *codec = avctx->codec;

    av_assert0(!av_strcasecmp(codec->name, "h264_mediacodec"));

    av_mediacodec_default_free(avctx);
#endif
}

const struct decoder decoder_ffmpeg_sw = {
    .name        = "ffmpeg_sw",
    .init        = ffdec_init_sw,
    .push_packet = ffdec_push_packet,
    .flush       = ffdec_flush,
};

const struct decoder decoder_ffmpeg_hw = {
    .name        = "ffmpeg_hw",
    .init        = ffdec_init_hw,
    .push_packet = ffdec_push_packet,
    .flush       = ffdec_flush,
    .uninit      = ffdec_uninit_hw,
};
