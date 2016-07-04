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

struct info_message {
    int width, height;
    int64_t duration;
};

struct async_context {
    void *log_ctx;

    struct demuxing_ctx  *demuxer;
    struct decoding_ctx  *decoder;
    struct filtering_ctx *filterer;

    pthread_t demuxer_tid;
    pthread_t decoder_tid;
    pthread_t filterer_tid;
    pthread_t main_tid;

    int demuxer_started;
    int decoder_started;
    int filterer_started;
    int main_started;

    AVThreadMessageQueue *src_queue;        // user     <-> demuxer
    AVThreadMessageQueue *pkt_queue;        // demuxer  <-> decoder
    AVThreadMessageQueue *frames_queue;     // decoder  <-> filterer
    AVThreadMessageQueue *sink_queue;       // filterer <-> user

    int thread_stack_size;
    int need_wait;

    int64_t request_seek;

    AVThreadMessageQueue *info_channel;
    struct info_message info;
    int has_info;

    int modules_initialized;
    const struct sxplayer_ctx *s;
};

static int fetch_mod_info(struct async_context *actx)
{
    int ret;
    struct message msg;

    TRACE(actx, "fetch module info");
    if (actx->has_info)
        return 0;
    async_start(actx);
    ret = av_thread_message_queue_recv(actx->info_channel, &msg, 0);
    if (ret < 0)
        return ret;
    av_assert0(msg.type = MSG_INFO);
    memcpy(&actx->info, msg.data, sizeof(actx->info));
    async_free_message_data(&msg);
    actx->has_info = 1;
    TRACE(actx, "info fetched");
    return 0;
}

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
    case MSG_INFO:
        av_freep(&msg->data);
        break;
    default:
        av_assert0(0);
    }
}

int64_t async_probe_duration(struct async_context *actx)
{
    int ret = fetch_mod_info(actx);
    if (ret < 0)
        return 0;
    return actx->info.duration;
}

int async_fetch_info(struct async_context *actx, struct sxplayer_info *info)
{
    int ret = fetch_mod_info(actx);
    if (ret < 0)
        return ret;
    info->width    = actx->info.width;
    info->height   = actx->info.height;
    info->duration = actx->info.duration * av_q2d(AV_TIME_BASE_Q);
    return 0;
}

static int send_seek_message(AVThreadMessageQueue *q, int64_t ts)
{
    int ret;
    struct message msg = {
        .type = MSG_SEEK,
        .data = av_malloc(sizeof(ts)),
    };

    if (!msg.data)
        return AVERROR(ENOMEM);
    *(int64_t *)msg.data = ts;

    // flush the queue so the seek message is processed ASAP (there might be a
    // previous seek message present)
    av_thread_message_flush(q);

    ret = av_thread_message_queue_send(q, &msg, 0);
    if (ret < 0) {
        av_thread_message_queue_set_err_recv(q, ret);
        av_freep(&msg.data);
    }
    return ret;
}

int async_seek(struct async_context *actx, int64_t ts)
{
    if (!actx->main_started || actx->need_wait)
        actx->request_seek = ts;
    else {
        int ret;

        ret = send_seek_message(actx->src_queue, ts);
        if (ret < 0) {
            actx->request_seek = ts;
        }
    }
    return 0;
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
    AVCodecParameters *par = avcodec_parameters_alloc();

    if ((s->filters && !filters) || !par) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* Demuxer */
    ret = demuxing_init(actx->log_ctx,
                        actx->demuxer,
                        actx->src_queue, actx->pkt_queue,
                        actx->info_channel,
                        s->filename, s->avselect,
                        s->pkt_skip_mod);
    if (ret < 0)
        goto end;

    /* Decoder */
    ret = decoding_init(actx->log_ctx,
                        actx->decoder,
                        actx->pkt_queue, actx->frames_queue,
                        demuxing_get_stream(actx->demuxer),
                        s->auto_hwaccel,
                        s->export_mvs,
                        s->opaque ? *(void **)s->opaque : NULL,
                        s->max_pixels);
    if (ret < 0)
        goto end;

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
    const AVCodecContext *avctx = decoding_get_avctx(actx->decoder);

    ret = avcodec_parameters_from_context(par, avctx);
    if (ret < 0)
        goto end;

    ret = filtering_init(actx->log_ctx,
                         actx->filterer,
                         actx->frames_queue, actx->sink_queue,
                         par, filters, s->sw_pix_fmt,
                         max_pts, s->max_pixels, s->audio_texture);
    if (ret < 0)
        goto end;

    int64_t duration = max_pts;
    const int64_t probe_duration = demuxing_probe_duration(actx->demuxer);

    av_assert0(AV_NOPTS_VALUE < 0);
    if (probe_duration != AV_NOPTS_VALUE && (duration <= 0 ||
                                             probe_duration < duration)) {
        LOG(s, INFO, "fix trim_duration from %f to %f",
              duration       * av_q2d(AV_TIME_BASE_Q),
              probe_duration * av_q2d(AV_TIME_BASE_Q));
        duration = probe_duration;
    }
    if (duration == AV_NOPTS_VALUE)
        duration = 0;
    struct info_message info = {
        .width    = avctx->width,
        .height   = avctx->height,
        .duration = duration,
    };

    TRACE(actx, "push module info");
    struct message msg = {
        .type = MSG_INFO,
        .data = av_memdup(&info, sizeof(info)),
    };
    if (!msg.data) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    av_thread_message_queue_set_err_recv(actx->info_channel, 0);
    ret = av_thread_message_queue_send(actx->info_channel, &msg, 0);
    if (ret < 0) {
        async_free_message_data(&msg);
        goto end;
    }
    TRACE(actx, "pushed info %dx%d d=%"PRId64, info.width, info.height, info.duration);

end:
    if (ret < 0)
        av_thread_message_queue_set_err_recv(actx->info_channel, ret);
    av_freep(&filters);
    avcodec_parameters_free(&par);
    return ret;
}

static int alloc_msg_queue(AVThreadMessageQueue **q, int n)
{
    int ret = av_thread_message_queue_alloc(q, n, sizeof(struct message));
    if (ret < 0)
        return ret;
    av_thread_message_queue_set_free_func(*q, async_free_message_data);
    return 0;
}

int async_init(struct async_context *actx, const struct sxplayer_ctx *s)
{
    int ret;

    actx->log_ctx = s->log_ctx;
    actx->s = s; // :(
    actx->thread_stack_size = s->thread_stack_size;
    actx->request_seek = AV_NOPTS_VALUE;

    TRACE(actx, "allocate queues");
    if ((ret = alloc_msg_queue(&actx->info_channel, 1))                 < 0 ||
        (ret = alloc_msg_queue(&actx->src_queue,    1))                 < 0 ||
        (ret = alloc_msg_queue(&actx->pkt_queue,    s->max_nb_packets)) < 0 ||
        (ret = alloc_msg_queue(&actx->frames_queue, s->max_nb_frames))  < 0 ||
        (ret = alloc_msg_queue(&actx->sink_queue,   s->max_nb_sink))    < 0)
        return ret;

    TRACE(actx, "create modules");
    actx->demuxer  = demuxing_alloc();
    actx->decoder  = decoding_alloc();
    actx->filterer = filtering_alloc();
    if (!actx->demuxer || !actx->decoder || !actx->filterer)
        return AVERROR(ENOMEM);

    return 0;
}

#define MODULE_THREAD_FUNC(name, action)                                        \
static void *name##_thread(void *arg)                                           \
{                                                                               \
    struct async_context *actx = arg;                                           \
    set_thread_name("sxp/" AV_STRINGIFY(name));                                 \
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
            LOG(actx, ERROR, "Unable to start " AV_STRINGIFY(name)              \
                " thread: %s", av_err2str(err));                                \
        } else                                                                  \
            actx->name##_started = 1;                                           \
    }                                                                           \
} while (0)

#define JOIN_MODULE_THREAD(name) do {                                           \
    if (!actx->name##_started) {                                                \
        TRACE(actx, "not joining " AV_STRINGIFY(name) " thread: not running");  \
    } else {                                                                    \
        TRACE(actx, "joining " AV_STRINGIFY(name) " thread");                   \
        int ret = pthread_join(actx->name##_tid, NULL);                         \
        if (ret)                                                                \
            LOG(actx, ERROR, "Unable to join " AV_STRINGIFY(name) ": %s",       \
                av_err2str(AVERROR(ret)));                                      \
        TRACE(actx, AV_STRINGIFY(name) " thread joined");                       \
        actx->name##_started = 0;                                               \
    }                                                                           \
} while (0)

MODULE_THREAD_FUNC(demuxer,  demuxing)
MODULE_THREAD_FUNC(decoder,  decoding)
MODULE_THREAD_FUNC(filterer, filtering)

/**
 * This thread is needed because the initialization of the modules can be slow
 * (for example, probing the information in the demuxer, or initializing the
 * decoder, in particular VideoToolbox) and we want the prefetch operation to
 * be as fast as possible for the user.
 */
static void *main_thread(void *arg)
{
    struct async_context *actx = arg;

    set_thread_name("sxp/main");

    LOG(actx, INFO, "starting main thread");

    if (!actx->modules_initialized) {
        int ret = initialize_modules(actx, actx->s);
        if (ret < 0) {
            LOG(actx, ERROR, "initializing modules failed with %s", av_err2str(ret));
            av_thread_message_queue_set_err_recv(actx->info_channel, ret);
            return NULL;
        }
        actx->modules_initialized = 1;
    }

    START_MODULE_THREAD(demuxer);
    START_MODULE_THREAD(decoder);
    START_MODULE_THREAD(filterer);

    TRACE(actx, "waiting for main to end");
    JOIN_MODULE_THREAD(filterer);
    JOIN_MODULE_THREAD(decoder);
    JOIN_MODULE_THREAD(demuxer);
    return NULL;
}

static int wait_main_thread(struct async_context *actx)
{
    JOIN_MODULE_THREAD(main);

    // every worker ended, reset queues states
    av_thread_message_queue_set_err_send(actx->src_queue,    0);
    av_thread_message_queue_set_err_send(actx->pkt_queue,    0);
    av_thread_message_queue_set_err_send(actx->frames_queue, 0);
    av_thread_message_queue_set_err_send(actx->sink_queue,   0);
    av_thread_message_queue_set_err_recv(actx->src_queue,    0);
    av_thread_message_queue_set_err_recv(actx->pkt_queue,    0);
    av_thread_message_queue_set_err_recv(actx->frames_queue, 0);
    av_thread_message_queue_set_err_recv(actx->sink_queue,   0);

    actx->need_wait = 0;
    return 0;
}

int async_start(struct async_context *actx)
{
    if (actx->need_wait)
        wait_main_thread(actx);
    else if (actx->main_started)
        return 0;

    if (actx->request_seek != AV_NOPTS_VALUE) {
        send_seek_message(actx->src_queue, actx->request_seek);
        actx->request_seek = AV_NOPTS_VALUE;
    }

    START_MODULE_THREAD(main);

    TRACE(actx, "async started");
    return 0;
}

int async_stop(struct async_context *actx)
{
    if (!actx)
        return 0;

    TRACE(actx, "stopping");

    // modules must stop feeding the queues
    av_thread_message_queue_set_err_send(actx->src_queue,    AVERROR_EXIT);
    av_thread_message_queue_set_err_send(actx->pkt_queue,    AVERROR_EXIT);
    av_thread_message_queue_set_err_send(actx->frames_queue, AVERROR_EXIT);
    av_thread_message_queue_set_err_send(actx->sink_queue,   AVERROR_EXIT);

    // ...and stop reading from them
    av_thread_message_queue_set_err_recv(actx->src_queue,    AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->pkt_queue,    AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->frames_queue, AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->sink_queue,   AVERROR_EXIT);

    return wait_main_thread(actx);
}

const char *async_get_msg_type_string(enum msg_type type)
{
    static const char * const s[NB_MSG] = {
        [MSG_FRAME]  = "frame",
        [MSG_PACKET] = "packet",
        [MSG_SEEK]   = "seek",
        [MSG_INFO]   = "info",
    };
    return s[type];
}

int async_pop_msg(struct async_context *actx, struct message *msg)
{
    async_start(actx);

    int ret = fetch_mod_info(actx); // make sure they are ready
    if (ret < 0)
        return ret;

    TRACE(actx, "fetching a message from the sink");
    ret = av_thread_message_queue_recv(actx->sink_queue, msg, 0);
    if (ret < 0) {
        TRACE(actx, "couldn't fetch message from sink because %s", av_err2str(ret));
        av_thread_message_queue_set_err_send(actx->sink_queue, ret);
        actx->need_wait = 1;
        return ret;
    }
    TRACE(actx, "got a message of type %s", async_get_msg_type_string(msg->type));
    return 0;
}

int async_started(const struct async_context *actx)
{
    return actx->main_started;
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

    av_thread_message_queue_free(&actx->src_queue);
    av_thread_message_queue_free(&actx->pkt_queue);
    av_thread_message_queue_free(&actx->frames_queue);
    av_thread_message_queue_free(&actx->sink_queue);

    av_thread_message_queue_free(&actx->info_channel);

    av_freep(actxp);
}
