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

#include <libavcodec/avcodec.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/threadmessage.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>

#include "internal.h"
#include "async.h"

#include "demuxing.h"
#include "decoding.h"
#include "filtering.h"

struct async_context {
    void *log_ctx;

    struct demuxing_ctx  *demuxer;
    struct decoding_ctx  *decoder;
    struct filtering_ctx *filterer;

    pthread_t demuxer_tid;
    pthread_t decoder_tid;
    pthread_t filterer_tid;

    int demuxer_started;
    int decoder_started;
    int filterer_started;
    int threads_started;

    AVThreadMessageQueue *pkt_queue;        // demuxer  <-> decoder
    AVThreadMessageQueue *frames_queue;     // decoder  <-> filterer
    AVThreadMessageQueue *sink_queue;       // filterer <-> user

    int thread_stack_size;
};

struct async_context *async_alloc_context(void)
{
    struct async_context *actx = av_mallocz(sizeof(*actx));
    if (!actx)
        return NULL;
    return actx;
}

void async_free_message_data(void *arg)
{
    struct message *msg = arg;

    switch (msg->type) {
    case MSG_FRAME: {
        AVFrame *frame = msg->data;
        av_frame_free(&frame);
        break;
    }
    case MSG_PACKET:
        av_packet_unref(msg->data);
        av_freep(&msg->data);
        break;
    case MSG_SEEK:
        av_freep(&msg->data);
        break;
    default:
        av_assert0(0);
    }
}

int64_t async_probe_duration(const struct async_context *actx)
{
    av_assert0(!actx->threads_started);
    return demuxing_probe_duration(actx->demuxer);
}

int async_fetch_info(const struct async_context *actx, struct sxplayer_info *info)
{
    const AVCodecContext *avctx = decoding_get_avctx(actx->decoder);
    info->width    = avctx->width;
    info->height   = avctx->height;
    info->duration = demuxing_probe_duration(actx->demuxer) * av_q2d(AV_TIME_BASE_Q);
    return 0;
}

int async_seek(struct async_context *actx, int64_t ts)
{
    return demuxing_seek(actx->demuxer, ts);
}

static char *update_filters_str(char *filters, const char *append)
{
    char *str;

    if (filters) {
        str = av_asprintf("%s,%s", filters, append);
        av_free(filters);
    } else {
        str = av_strdup(append);
    }
    return str;
}

static int initialize_modules(struct async_context *actx,
                              const struct sxplayer_ctx *s)
{
    int ret;
    char *filters = av_strdup(s->filters);

    // XXX filters memleak
    if (!filters && s->filters)
        return AVERROR(ENOMEM);

    /* Demuxer */
    ret = demuxing_init(actx->log_ctx,
                        actx->demuxer,
                        actx->pkt_queue,
                        s->filename, s->avselect,
                        s->pkt_skip_mod);
    if (ret < 0)
        return ret;

    /* Decoder */
    ret = decoding_init(actx->log_ctx,
                        actx->decoder,
                        actx->pkt_queue, actx->frames_queue,
                        demuxing_get_stream(actx->demuxer),
                        s->auto_hwaccel,
                        s->export_mvs);
    if (ret < 0)
        return ret;

    /* Filterer */
    if (s->autorotate) {
        const double theta = demuxing_probe_rotation(actx->demuxer);

        if (fabs(theta - 90) < 1.0)
            filters = update_filters_str(filters, "transpose=clock");
        else if (fabs(theta - 180) < 1.0)
            filters = update_filters_str(filters, "vflip,hflip");
        else if (fabs(theta - 270) < 1.0)
            filters = update_filters_str(filters, "transpose=cclock");
        TRACE(actx, "update filtergraph to: %s", filters);
    }

    const int64_t max_pts = s->trim_duration64 >= 0 ? s->skip64 + s->trim_duration64
                                                    : AV_NOPTS_VALUE;
    ret = filtering_init(actx->log_ctx,
                         actx->filterer,
                         actx->frames_queue, actx->sink_queue,
                         decoding_get_avctx(actx->decoder),
                         filters, s->sw_pix_fmt, max_pts);
    if (ret < 0)
        return ret;

    return 0;
}

int async_init(struct async_context *actx, const struct sxplayer_ctx *s)
{
    int ret;

    actx->log_ctx = s->log_ctx;
    actx->thread_stack_size = s->thread_stack_size;

    TRACE(actx, "allocate queues");
    ret = av_thread_message_queue_alloc(&actx->pkt_queue, s->max_nb_packets,
                                        sizeof(struct message));
    if (ret < 0)
        return ret;

    ret = av_thread_message_queue_alloc(&actx->frames_queue, s->max_nb_frames,
                                        sizeof(struct message));
    if (ret < 0)
        return ret;

    ret = av_thread_message_queue_alloc(&actx->sink_queue, s->max_nb_sink,
                                        sizeof(struct message));
    if (ret < 0)
        return ret;

    av_thread_message_queue_set_free_func(actx->pkt_queue,    async_free_message_data);
    av_thread_message_queue_set_free_func(actx->frames_queue, async_free_message_data);
    av_thread_message_queue_set_free_func(actx->sink_queue,   async_free_message_data);

    TRACE(actx, "create modules");
    actx->demuxer  = demuxing_alloc();
    actx->decoder  = decoding_alloc();
    actx->filterer = filtering_alloc();
    if (!actx->demuxer || !actx->decoder || !actx->filterer)
        return AVERROR(ENOMEM);

    ret = initialize_modules(actx, s);
    if (ret < 0)
        return ret;

    TRACE(actx, "init sucessful");
    return 0;
}

#define MODULE_THREAD_FUNC(name, action)                                        \
static void *name##_thread(void *arg)                                           \
{                                                                               \
    struct async_context *actx = arg;                                           \
    set_thread_name("sxp-" AV_STRINGIFY(name));                                 \
    TRACE(actx, "[>] " AV_STRINGIFY(action) " thread starting");                \
    action##_run(actx->name);                                                   \
    TRACE(actx, "[<] " AV_STRINGIFY(action) " thread ending");                  \
    return NULL;                                                                \
}

#define START_MODULE_THREAD(name) do {                                          \
    if (actx->name##_started) {                                                 \
        TRACE(actx, "not starting " AV_STRINGIFY(name)                          \
              " thread: already running");                                      \
    } else {                                                                    \
        pthread_attr_t attr;                                                    \
        pthread_attr_t *attrp = NULL;                                           \
        if (actx->thread_stack_size > 0) {                                      \
            pthread_attr_init(&attr);                                           \
            if (ENABLE_DBG) {                                                   \
                size_t stack_size;                                              \
                pthread_attr_getstacksize(&attr, &stack_size);                  \
                TRACE(actx, "stack size before: %zd", stack_size);              \
                pthread_attr_setstacksize(&attr, actx->thread_stack_size);      \
                stack_size = 0;                                                 \
                pthread_attr_getstacksize(&attr, &stack_size);                  \
                TRACE(actx, "stack size after: %zd", stack_size);               \
            } else {                                                            \
                pthread_attr_setstacksize(&attr, actx->thread_stack_size);      \
            }                                                                   \
            attrp = &attr;                                                      \
        }                                                                       \
        int ret = pthread_create(&actx->name##_tid, attrp, name##_thread, actx);\
        if (attrp)                                                              \
            pthread_attr_destroy(attrp);                                        \
        if (ret) {                                                              \
            const int err = AVERROR(ret);                                       \
            LOG_ERROR(actx, "Unable to start " AV_STRINGIFY(name)               \
                      " thread: %s", av_err2str(err));                          \
            return err;                                                         \
        }                                                                       \
        actx->name##_started = 1;                                               \
    }                                                                           \
} while (0)

#define JOIN_MODULE_THREAD(name) do {                                           \
    if (!actx->name##_started) {                                                \
        TRACE(actx, "not joining " AV_STRINGIFY(name) " thread: not running");  \
    } else {                                                                    \
        TRACE(actx, "joining " AV_STRINGIFY(name) " thread");                   \
        int ret = pthread_join(actx->name##_tid, NULL);                         \
        if (ret)                                                                \
            LOG_ERROR(actx, "Unable to join " AV_STRINGIFY(name) ": %s",        \
                      av_err2str(AVERROR(ret)));                                \
        TRACE(actx, AV_STRINGIFY(name) " thread joined");                       \
        actx->name##_started = 0;                                               \
    }                                                                           \
} while (0)

MODULE_THREAD_FUNC(demuxer,  demuxing)
MODULE_THREAD_FUNC(decoder,  decoding)
MODULE_THREAD_FUNC(filterer, filtering)

int async_start(struct async_context *actx)
{
    if (actx->threads_started)
        return 0;

    TRACE(actx, "starting threads");
    START_MODULE_THREAD(demuxer);
    START_MODULE_THREAD(decoder);
    START_MODULE_THREAD(filterer);
    actx->threads_started = 1;
    return 0;
}

int async_wait(struct async_context *actx)
{
    TRACE(actx, "waiting for threads to end");
    JOIN_MODULE_THREAD(filterer);
    JOIN_MODULE_THREAD(decoder);
    JOIN_MODULE_THREAD(demuxer);

    // every worker ended, reset queues states
    av_thread_message_queue_set_err_send(actx->pkt_queue,    0);
    av_thread_message_queue_set_err_send(actx->frames_queue, 0);
    av_thread_message_queue_set_err_send(actx->sink_queue,   0);
    av_thread_message_queue_set_err_recv(actx->pkt_queue,    0);
    av_thread_message_queue_set_err_recv(actx->frames_queue, 0);
    av_thread_message_queue_set_err_recv(actx->sink_queue,   0);

    actx->threads_started = 0;
    return 0;
}

int async_stop(struct async_context *actx)
{
    if (!actx)
        return 0;

    TRACE(actx, "stopping");

    // modules must stop feeding the queues
    av_thread_message_queue_set_err_send(actx->pkt_queue,    AVERROR_EXIT);
    av_thread_message_queue_set_err_send(actx->frames_queue, AVERROR_EXIT);
    av_thread_message_queue_set_err_send(actx->sink_queue,   AVERROR_EXIT);

    // ...and stop reading from them
    av_thread_message_queue_set_err_recv(actx->pkt_queue,    AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->frames_queue, AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->sink_queue,   AVERROR_EXIT);

    return async_wait(actx);
}

const char *async_get_msg_type_string(enum msg_type type)
{
    static const char * const s[] = {
        [MSG_FRAME]  = "frame",
        [MSG_PACKET] = "packet",
        [MSG_SEEK]   = "seek"
    };
    return s[type];
}

int async_pop_msg(struct async_context *actx, struct message *msg)
{
    TRACE(actx, "fetching a message from the sink");
    av_assert0(actx->threads_started);
    int ret = av_thread_message_queue_recv(actx->sink_queue, msg, 0);
    if (ret < 0) {
        TRACE(actx, "couldn't fetch message from sink because %s", av_err2str(ret));
        av_thread_message_queue_set_err_send(actx->sink_queue, ret);
        return ret;
    }
    TRACE(actx, "got a message of type %s", async_get_msg_type_string(msg->type));
    return 0;
}

int async_started(const struct async_context *actx)
{
    return actx->threads_started;
}

void async_free(struct async_context **actxp)
{
    struct async_context *actx = *actxp;

    if (!actx)
        return;

    async_stop(actx);

    demuxing_free(&actx->demuxer);
    decoding_free(&actx->decoder);
    filtering_free(&actx->filterer);

    av_thread_message_queue_free(&actx->pkt_queue);
    av_thread_message_queue_free(&actx->frames_queue);
    av_thread_message_queue_free(&actx->sink_queue);

    av_freep(actxp);
}
