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
#include <libavutil/opt.h>
#include <libavutil/avassert.h>

#include "decoding.h"
#include "decoders.h"
#include "internal.h"

static int ffdec_init(struct decoder_ctx *ctx)
{
    int ret;
    AVCodecContext *avctx = ctx->avctx;
    AVCodec *dec = avcodec_find_decoder(avctx->codec_id);

    TRACE(ctx, "initialize context");

    av_opt_set_int(avctx, "refcounted_frames", 1, 0);

    if (avctx->codec_id == AV_CODEC_ID_H264) {
        AVCodec *codec = avcodec_find_decoder_by_name("h264_mediacodec");
        if (codec)
            dec = codec;
    }

    TRACE(ctx, "codec open");

    ret = avcodec_open2(avctx, dec, NULL);
    if (ret < 0)
        return ret;

    ctx->avctx = avctx;

    return ret;
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

const struct decoder decoder_ffmpeg = {
    .name        = "ffmpeg",
    .init        = ffdec_init,
    .push_packet = ffdec_push_packet,
    .flush       = ffdec_flush,
};
