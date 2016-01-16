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

#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/display.h>
#include <libavutil/eval.h>

#include "demuxing.h"
#include "internal.h"

struct demuxing_ctx {
    const AVClass *class;
    pthread_mutex_t lock;
    int pkt_skip_mod;
    int64_t request_seek;
    int64_t pkt_count;
    AVFormatContext *fmt_ctx;
    AVStream *stream;
    int stream_idx;
    AVThreadMessageQueue *pkt_queue;
};

static const AVClass demuxing_class = {
    .class_name = "demuxing",
    .item_name  = av_default_item_name,
};

struct demuxing_ctx *demuxing_alloc(void)
{
    struct demuxing_ctx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->class = &demuxing_class;
    return ctx;
}

// XXX: we should probably prefer the stream duration over the format
// duration
int64_t demuxing_probe_duration(const struct demuxing_ctx *ctx)
{
    if (!strstr(ctx->fmt_ctx->iformat->name, "image2") && !strstr(ctx->fmt_ctx->iformat->name, "_pipe")) {
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

double demuxing_probe_rotation(const struct demuxing_ctx *ctx)
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

const AVStream *demuxing_get_stream(const struct demuxing_ctx *ctx)
{
    return ctx->stream;
}

int demuxing_init(struct demuxing_ctx *ctx,
                  AVThreadMessageQueue *pkt_queue,
                  const char *filename, int avselect,
                  int pkt_skip_mod)
{
    enum AVMediaType media_type;

    int ret = pthread_mutex_init(&ctx->lock, NULL);
    if (ret < 0)
        return AVERROR(ret);

    ctx->pkt_queue = pkt_queue;
    ctx->pkt_skip_mod = pkt_skip_mod;
    ctx->request_seek = AV_NOPTS_VALUE;

    switch (avselect) {
    case SXPLAYER_SELECT_VIDEO: media_type = AVMEDIA_TYPE_VIDEO; break;
    case SXPLAYER_SELECT_AUDIO: media_type = AVMEDIA_TYPE_AUDIO; break;
    default:
        av_assert0(0);
    }

    TRACE(ctx, "opening %s", filename);
    ret = avformat_open_input(&ctx->fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Unable to open input file '%s'\n", filename);
        return ret;
    }

    /* Evil hack: we make sure avformat_find_stream_info() doesn't decode any
     * video, because it will use the software decoder, which will be slow and
     * use an indecent amount of memory (15-20MB). It slows down startup
     * significantly on embedded platforms and risks a kill from the OOM
     * because of the large and fast amount of allocated memory.
     *
     * The max_analyze_duration is accessible through avoptions, but a negative
     * value isn't actually in the allowed range, so we directly mess with the
     * field. We can't unfortunately set it to 0, because at the moment of
     * writing this code 0 (which is the default value) means "auto", which
     * will be then set to something like 5 seconds for video. */
    ctx->fmt_ctx->max_analyze_duration = -1;

    TRACE(ctx, "find stream info");
    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Unable to find input stream information\n");
        return ret;
    }

    TRACE(ctx, "find best stream");
    ret = av_find_best_stream(ctx->fmt_ctx, media_type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Unable to find a %s stream in the input file\n",
                av_get_media_type_string(media_type));
        return ret;
    }
    ctx->stream_idx = ret;
    ctx->stream = ctx->fmt_ctx->streams[ctx->stream_idx];

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

int demuxing_seek(struct demuxing_ctx *ctx, int64_t ts)
{
    TRACE(ctx, "request seek at ts=%s", PTS2TIMESTR(ts));
    int ret = AVERROR(pthread_mutex_lock(&ctx->lock));
    if (ret < 0)
        return ret;
    ctx->request_seek = ts;
    ret = AVERROR(pthread_mutex_unlock(&ctx->lock));
    if (ret < 0)
        return ret;
    return 0;
}

static int push_seek_message(AVThreadMessageQueue *q, int64_t ts)
{
    int ret;
    struct message msg = {
        .type = MSG_SEEK,
        .data = av_malloc(sizeof(ts)),
    };

    if (!msg.data)
        return AVERROR(ENOMEM);
    *(int64_t *)msg.data = ts;

    // flush the queue so the seek message is processed ASAP
    av_thread_message_flush(q);

    ret = av_thread_message_queue_send(q, &msg, 0);
    if (ret < 0) {
        av_thread_message_queue_set_err_recv(q, ret);
        av_freep(&msg.data);
    }
    return ret;
}

void demuxing_run(struct demuxing_ctx *ctx)
{
    int ret;

    TRACE(ctx, "demuxing packets in queue %p", ctx->pkt_queue);

    for (;;) {
        int64_t seek_to;
        AVPacket pkt;
        struct message msg = {.type = MSG_PACKET};

        /* get seek value and reset request */
        ret = AVERROR(pthread_mutex_lock(&ctx->lock));
        if (ret < 0)
            break;
        seek_to = ctx->request_seek;
        ctx->request_seek = AV_NOPTS_VALUE;
        ret = AVERROR(pthread_mutex_unlock(&ctx->lock));
        if (ret < 0)
            break;

        if (seek_to != AV_NOPTS_VALUE) {

            /* notify the decoder about the seek by using its pkt queue */
            TRACE(ctx, "forward seek message (to %s) to decoder",
                  PTS2TIMESTR(seek_to));
            ret = push_seek_message(ctx->pkt_queue, seek_to);
            if (ret < 0)
                break;

            /* do actual seek so the following packet that will be pulled in
             * this current thread will be at the (approximate) requested time */
            INFO(ctx, "Seek in media at ts=%s", PTS2TIMESTR(seek_to));
            ret = avformat_seek_file(ctx->fmt_ctx, -1, INT64_MIN, seek_to, seek_to, 0);
            if (ret < 0)
                break;
        }

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
                av_log(ctx, AV_LOG_ERROR, "Unable to send packet to decoder: %s\n", av_err2str(ret));
            TRACE(ctx, "can't send pkt to decoder: %s", av_err2str(ret));
            av_thread_message_queue_set_err_recv(ctx->pkt_queue, ret);
            break;
        }
    }

    if (!ret)
        ret = AVERROR_EOF;
    TRACE(ctx, "notify decoder about %s", av_err2str(ret));
    av_thread_message_queue_set_err_recv(ctx->pkt_queue, ret);
}

void demuxing_free(struct demuxing_ctx **ctxp)
{
    struct demuxing_ctx *ctx = *ctxp;
    if (!ctx)
        return;
    pthread_mutex_destroy(&ctx->lock);
    avformat_close_input(&ctx->fmt_ctx);
    av_freep(ctxp);
}
