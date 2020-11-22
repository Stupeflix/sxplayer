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

#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/display.h>
#include <libavutil/eval.h>

#include "mod_demuxing.h"
#include "internal.h"
#include "log.h"
#include "msg.h"

struct demuxing_ctx {
    void *log_ctx;
    int pkt_skip_mod;
    int64_t pkt_count;
    AVFormatContext *fmt_ctx;
    AVStream *stream;
    int stream_idx;
    int is_image;
    AVThreadMessageQueue *src_queue;
    AVThreadMessageQueue *pkt_queue;
};

struct demuxing_ctx *sxpi_demuxing_alloc(void)
{
    struct demuxing_ctx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    return ctx;
}

// XXX: we should probably prefer the stream duration over the format
// duration
int64_t sxpi_demuxing_probe_duration(const struct demuxing_ctx *ctx)
{
    if (!ctx->is_image) {
        int64_t probe_duration64 = ctx->fmt_ctx->duration;
        AVRational scaleq = AV_TIME_BASE_Q;

        if (probe_duration64 == AV_NOPTS_VALUE && ctx->stream->time_base.den) {
            probe_duration64 = ctx->stream->duration;
            scaleq = ctx->stream->time_base;
        }

        if (probe_duration64 != AV_NOPTS_VALUE)
            return av_rescale_q_rnd(probe_duration64, scaleq, AV_TIME_BASE_Q, 0);
    }
    return AV_NOPTS_VALUE;
}

double sxpi_demuxing_probe_rotation(const struct demuxing_ctx *ctx)
{
    AVStream *st = (AVStream *)ctx->stream; // XXX: Fix FFmpeg.
    AVDictionaryEntry *rotate_tag = av_dict_get(st->metadata, "rotate", NULL, 0);
    const uint8_t *displaymatrix = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, NULL);
    double theta = 0;

    if (rotate_tag && *rotate_tag->value && strcmp(rotate_tag->value, "0")) {
        char *tail;
        theta = av_strtod(rotate_tag->value, &tail);
        if (*tail)
            theta = 0;
    }
    if (displaymatrix && !theta)
        theta = -av_display_rotation_get((const int32_t *)displaymatrix);
    theta -= 360*floor(theta/360 + 0.9/360);
    return theta;
}

const AVStream *sxpi_demuxing_get_stream(const struct demuxing_ctx *ctx)
{
    return ctx->stream;
}

int sxpi_demuxing_is_image(const struct demuxing_ctx *ctx)
{
    return ctx->is_image;
}

int sxpi_demuxing_init(void *log_ctx,
                       struct demuxing_ctx *ctx,
                       AVThreadMessageQueue *src_queue,
                       AVThreadMessageQueue *pkt_queue,
                       const char *filename,
                       const struct sxplayer_opts *opts)
{
    enum AVMediaType media_type;

    ctx->log_ctx = log_ctx;

    ctx->src_queue = src_queue;
    ctx->pkt_queue = pkt_queue;
    ctx->pkt_skip_mod = opts->pkt_skip_mod;

    switch (opts->avselect) {
    case SXPLAYER_SELECT_VIDEO: media_type = AVMEDIA_TYPE_VIDEO; break;
    case SXPLAYER_SELECT_AUDIO: media_type = AVMEDIA_TYPE_AUDIO; break;
    default:
        av_assert0(0);
    }

    TRACE(ctx, "opening %s", filename);
    int ret = avformat_open_input(&ctx->fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to open input file '%s'", filename);
        return ret;
    }

    TRACE(ctx, "find stream info");
    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to find input stream information");
        return ret;
    }

    TRACE(ctx, "find best stream");
    ret = av_find_best_stream(ctx->fmt_ctx, media_type, opts->stream_idx, -1, NULL, 0);
    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to find a %s stream in the input file",
            av_get_media_type_string(media_type));
        return ret;
    }
    ctx->stream_idx = ret;
    ctx->stream = ctx->fmt_ctx->streams[ctx->stream_idx];
    ctx->is_image = strstr(ctx->fmt_ctx->iformat->name, "image2") ||
                    strstr(ctx->fmt_ctx->iformat->name, "_pipe");
    LOG(ctx, INFO, "Selected %s stream %d",
        av_get_media_type_string(media_type), ctx->stream_idx);

    /* Automatically discard all the other streams so we don't have to filter
     * them out most of the time */
    for (int i = 0; i < ctx->fmt_ctx->nb_streams; i++)
        if (i != ctx->stream_idx)
            ctx->fmt_ctx->streams[i]->discard = AVDISCARD_ALL;

    av_dump_format(ctx->fmt_ctx, 0, filename, 0);

    return 0;
}

static int pull_packet(struct demuxing_ctx *ctx, AVPacket *pkt)
{
    int ret;
    AVFormatContext *fmt_ctx = ctx->fmt_ctx;
    const int target_stream_idx = ctx->stream->index;

    for (;;) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0)
            break;

        if (pkt->stream_index != target_stream_idx) {
            TRACE(ctx, "pkt->idx=%d vs %d",
                  pkt->stream_index, target_stream_idx);
            av_packet_unref(pkt);
            continue;
        }

        if (ctx->pkt_skip_mod) {
            ctx->pkt_count++;
            if (ctx->pkt_count % ctx->pkt_skip_mod && !(pkt->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(pkt);
                continue;
            }
        }

        break;
    }

    TRACE(ctx, "packet ret %s", av_err2str(ret));
    return ret;
}

void sxpi_demuxing_run(struct demuxing_ctx *ctx)
{
    int ret;
    int in_err, out_err;

    TRACE(ctx, "demuxing packets in queue %p", ctx->pkt_queue);

    for (;;) {
        AVPacket pkt;
        struct message msg;

        ret = av_thread_message_queue_recv(ctx->src_queue, &msg, AV_THREAD_MESSAGE_NONBLOCK);
        if (ret != AVERROR(EAGAIN)) {
            if (ret < 0)
                break;

            if (msg.type == MSG_SEEK) {
                av_assert0(!ctx->is_image);

                /* Make later modules stop working ASAP */
                av_thread_message_flush(ctx->pkt_queue);

                /* do actual seek so the following packet that will be pulled in
                 * this current thread will be at the (approximate) requested time */
                const int64_t seek_to = *(int64_t *)msg.data;
                LOG(ctx, INFO, "Seek in media at ts=%s", PTS2TIMESTR(seek_to));
                ret = avformat_seek_file(ctx->fmt_ctx, -1, INT64_MIN, seek_to, seek_to, 0);
                if (ret < 0) {
                    sxpi_msg_free_data(&msg);
                    break;
                }
            }

            /* Forward the message */
            ret = av_thread_message_queue_send(ctx->pkt_queue, &msg, 0);
            if (ret < 0) {
                sxpi_msg_free_data(&msg);
                break;
            }
        }

        msg.type = MSG_PACKET;

        ret = pull_packet(ctx, &pkt);
        if (ret < 0)
            break;

        TRACE(ctx, "pulled a packet of size %d, sending to decoder", pkt.size);

        msg.data = av_memdup(&pkt, sizeof(pkt));
        if (!msg.data) {
            av_packet_unref(&pkt);
            break;
        }

        ret = av_thread_message_queue_send(ctx->pkt_queue, &msg, 0);
        TRACE(ctx, "sent packet to decoder, ret=%s", av_err2str(ret));

        if (ret < 0) {
            av_packet_unref(&pkt);
            av_freep(&msg.data);
            if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
                LOG(ctx, ERROR, "Unable to send packet to decoder: %s", av_err2str(ret));
            TRACE(ctx, "can't send pkt to decoder: %s", av_err2str(ret));
            av_thread_message_queue_set_err_recv(ctx->pkt_queue, ret);
            break;
        }
    }

    if (ret < 0 && ret != AVERROR_EOF) {
        in_err = out_err = ret;
    } else {
        in_err = AVERROR_EXIT;
        out_err = AVERROR_EOF;
    }
    TRACE(ctx, "notify user with %s and decoder with %s",
          av_err2str(in_err), av_err2str(out_err));
    av_thread_message_queue_set_err_send(ctx->src_queue, in_err);
    av_thread_message_flush(ctx->src_queue);
    av_thread_message_queue_set_err_recv(ctx->pkt_queue, out_err);
}

void sxpi_demuxing_free(struct demuxing_ctx **ctxp)
{
    struct demuxing_ctx *ctx = *ctxp;
    if (!ctx)
        return;
    avformat_close_input(&ctx->fmt_ctx);
    av_freep(ctxp);
}
