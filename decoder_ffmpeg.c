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

#include <stdio.h>
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

#if HAVE_MEDIACODEC_HWACCEL
#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext_mediacodec.h>

static int init_mediacodec(struct decoder_ctx *ctx)
{
    AVCodecContext *avctx = ctx->avctx;

    if (avctx->codec_id != AV_CODEC_ID_H264  &&
        avctx->codec_id != AV_CODEC_ID_HEVC  &&
        avctx->codec_id != AV_CODEC_ID_MPEG4 &&
        avctx->codec_id != AV_CODEC_ID_VP8   &&
        avctx->codec_id != AV_CODEC_ID_VP9)
        return AVERROR_DECODER_NOT_FOUND;

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

    AVBufferRef *hw_device_ctx_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!hw_device_ctx_ref)
        return -1;

    AVHWDeviceContext *hw_device_ctx = (AVHWDeviceContext *)hw_device_ctx_ref->data;
    AVMediaCodecDeviceContext *hw_ctx = hw_device_ctx->hwctx;
    hw_ctx->surface = ctx->opaque;

    int ret = av_hwdevice_ctx_init(hw_device_ctx_ref);
    if (ret < 0) {
        av_buffer_unref(&hw_device_ctx_ref);
        return ret;
    }

    avctx->hw_device_ctx = hw_device_ctx_ref;
    avctx->thread_count = 1;

    AVDictionary *opts = NULL;
    av_dict_set_int(&opts, "delay_flush", 1, 0);

    ret = avcodec_open2(avctx, codec, &opts);
    if (ret < 0) {
        av_buffer_unref(&avctx->hw_device_ctx);
    }
    av_dict_free(&opts);
    return ret;
}
#endif

#if HAVE_VAAPI_HWACCEL
#include <libavutil/hwcontext_vaapi.h>

static int init_vaapi(struct decoder_ctx *ctx)
{
    AVCodecContext *avctx = ctx->avctx;

    if (avctx->codec_id != AV_CODEC_ID_H264 &&
        avctx->codec_id != AV_CODEC_ID_HEVC)
        return AVERROR_DECODER_NOT_FOUND;

    if (!ctx->opaque)
        return AVERROR_DECODER_NOT_FOUND;

    AVBufferRef *hw_device_ctx_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    if (!hw_device_ctx_ref)
        return -1;

    AVHWDeviceContext *hw_device_ctx = (AVHWDeviceContext *)hw_device_ctx_ref->data;
    AVVAAPIDeviceContext *hw_ctx = hw_device_ctx->hwctx;
    hw_ctx->display = ctx->opaque;

    int ret = av_hwdevice_ctx_init(hw_device_ctx_ref);
    if (ret < 0) {
        av_buffer_unref(&hw_device_ctx_ref);
        return ret;
    }

    avctx->hw_device_ctx = hw_device_ctx_ref;
    avctx->thread_count = 1;

    AVCodec *codec = avcodec_find_decoder(avctx->codec_id);
    ret = avcodec_open2(avctx, codec, NULL);
    if (ret < 0) {
        av_buffer_unref(&avctx->hw_device_ctx);
    }
    return ret;
}
#endif

static int ffdec_init_sw(struct decoder_ctx *ctx, const struct sxplayer_opts *opts)
{
    AVCodecContext *avctx = ctx->avctx;
    avctx->thread_count = 0;

    AVCodec *codec = avcodec_find_decoder(avctx->codec_id);
    return avcodec_open2(avctx, codec, NULL);
}

static int ffdec_init_hw(struct decoder_ctx *ctx, const struct sxplayer_opts *opts)
{
#if HAVE_MEDIACODEC_HWACCEL
    return init_mediacodec(ctx);
#elif HAVE_VAAPI_HWACCEL
    return init_vaapi(ctx);
#endif
    return AVERROR_DECODER_NOT_FOUND;
}

static int ffdec_push_packet(struct decoder_ctx *ctx, const AVPacket *pkt)
{
    int ret;
    int pkt_consumed = 0;
    const int flush = !pkt->size;
    AVCodecContext *avctx = ctx->avctx;

    av_assert0(avctx->codec_type == AVMEDIA_TYPE_VIDEO ||
               avctx->codec_type == AVMEDIA_TYPE_AUDIO);

    TRACE(ctx, "Received packet of size %d", pkt->size);

    while (!pkt_consumed) {
        ret = avcodec_send_packet(avctx, pkt);
        if (ret == AVERROR(EAGAIN)) {
            ret = 0;
        } else if (ret < 0) {
            LOG(ctx, ERROR, "Error sending packet to %s decoder: %s",
                av_get_media_type_string(avctx->codec_type),
                av_err2str(ret));
            return ret;
        } else {
            pkt_consumed = 1;
        }

        const int draining = flush && pkt_consumed;
        while (ret >= 0 || (draining && ret == AVERROR(EAGAIN))) {
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
};
