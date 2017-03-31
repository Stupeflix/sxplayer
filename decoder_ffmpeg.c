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

#include "mod_decoding.h"
#include "decoders.h"
#include "internal.h"
#include "log.h"

#ifndef HAVE_MEDIACODEC_HWACCEL
#error "HAVE_MEDIACODEC_HWACCEL must be defined"
#endif

#if HAVE_MEDIACODEC_HWACCEL

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
                av_freep(&mediacodec_ctx);
                continue;
            }

            ctx->use_hwaccel = 1;
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

    ctx->use_hwaccel = 0;
    av_opt_set_int(avctx, "refcounted_frames", 1, 0);

    if (hw) {
        if (avctx->codec_id == AV_CODEC_ID_H264  ||
            avctx->codec_id == AV_CODEC_ID_HEVC  ||
            avctx->codec_id == AV_CODEC_ID_MPEG4 ||
            avctx->codec_id == AV_CODEC_ID_VP8   ||
            avctx->codec_id == AV_CODEC_ID_VP9) {
            const char *codec_name = NULL;

            switch (avctx->codec_id) {
            case AV_CODEC_ID_H264:
                codec_name = "h264_mediacodec";
                break;
            case AV_CODEC_ID_HEVC:
                codec_name = "hevc_mediacodec";
                break;
            case AV_CODEC_ID_MPEG4:
                codec_name = "mpeg4_mediacodec";
                break;
            case AV_CODEC_ID_VP8:
                codec_name = "vp8_mediacodec";
                break;
            case AV_CODEC_ID_VP9:
                codec_name = "vp9_mediacodec";
                break;
            default:
                av_assert0(0);
            }

            AVCodec *codec = avcodec_find_decoder_by_name(codec_name);
            if (!codec)
                return AVERROR_DECODER_NOT_FOUND;

#if HAVE_MEDIACODEC_HWACCEL
            avctx->opaque = ctx;
            avctx->get_format = mediacodec_hwaccel_get_format;
            avctx->thread_count = 1;
#endif
            dec = codec;
        } else {
            return AVERROR_DECODER_NOT_FOUND;
        }
    }

    TRACE(ctx, "codec open");

    ret = avcodec_open2(avctx, dec, NULL);
    if (ret < 0) {
#if HAVE_MEDIACODEC_HWACCEL
        if (ctx->use_hwaccel) {
            av_mediacodec_default_free(avctx);
            ctx->use_hwaccel = 0;
        }
#endif
        return ret;
    }

    ctx->avctx = avctx;

    return ret;
}

static int ffdec_init_sw(struct decoder_ctx *ctx, const struct sxplayer_opts *opts)
{
    return ffdec_init(ctx, 0);
}

static int ffdec_init_hw(struct decoder_ctx *ctx, const struct sxplayer_opts *opts)
{
    return ffdec_init(ctx, 1);
}

static int ffdec_push_packet(struct decoder_ctx *ctx, const AVPacket *pkt)
{
    int ret;
    const int flush = !pkt->size;
    AVCodecContext *avctx = ctx->avctx;

    av_assert0(avctx->codec_type == AVMEDIA_TYPE_VIDEO ||
               avctx->codec_type == AVMEDIA_TYPE_AUDIO);

    TRACE(ctx, "Received packet of size %d", pkt->size);

    ret = avcodec_send_packet(avctx, pkt);
    if (ret < 0) {
        LOG(ctx, ERROR, "Error sending packet to %s decoder: %s",
            av_get_media_type_string(avctx->codec_type),
            av_err2str(ret));
        return ret;
    }

    while (ret >= 0) {
        AVFrame *dec_frame = av_frame_alloc();

        if (!dec_frame)
            return AVERROR(ENOMEM);

        ret = avcodec_receive_frame(avctx, dec_frame);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            LOG(ctx, ERROR, "Error receiving frame from %s decoder: %s",
                av_get_media_type_string(avctx->codec_type),
                av_err2str(ret));
                av_frame_free(&dec_frame);
            return ret;
        }

        if (ret >= 0) {
            ret = sxpi_decoding_queue_frame(ctx->decoding_ctx, dec_frame);
            if (ret < 0) {
                TRACE(ctx, "Could not queue frame: %s", av_err2str(ret));
                av_frame_free(&dec_frame);
                return ret;
            }
        } else {
            av_frame_free(&dec_frame);
        }
    }
    if (ret == AVERROR(EAGAIN))
        ret = 0;

    if (flush)
        ret = sxpi_decoding_queue_frame(ctx->decoding_ctx, NULL);

    return ret;
}

static void ffdec_flush(struct decoder_ctx *ctx)
{
    AVCodecContext *avctx = ctx->avctx;
    avcodec_flush_buffers(avctx);
}


static void ffdec_uninit_hw(struct decoder_ctx *ctx)
{
    if (ctx->use_hwaccel) {
#if HAVE_MEDIACODEC_HWACCEL
        AVCodecContext *avctx = ctx->avctx;
        av_mediacodec_default_free(avctx);
#endif
    }
}

const struct decoder sxpi_decoder_ffmpeg_sw = {
    .name        = "ffmpeg_sw",
    .init        = ffdec_init_sw,
    .push_packet = ffdec_push_packet,
    .flush       = ffdec_flush,
};

const struct decoder sxpi_decoder_ffmpeg_hw = {
    .name        = "ffmpeg_hw",
    .init        = ffdec_init_hw,
    .push_packet = ffdec_push_packet,
    .flush       = ffdec_flush,
    .uninit      = ffdec_uninit_hw,
};
