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
#include "log.h"

#include "mod_demuxing.h"
#include "mod_decoding.h"
#include "mod_filtering.h"

struct info_message {
    int width, height;
    int64_t duration;
    int is_image;
    AVRational timebase;
};

struct async_context {
    void *log_ctx;
    const char *filename;
    const struct sxplayer_opts *o;

    struct demuxing_ctx  *demuxer;
    struct decoding_ctx  *decoder;
    struct filtering_ctx *filterer;

    pthread_t demuxer_tid;
    pthread_t decoder_tid;
    pthread_t filterer_tid;
    pthread_t control_tid;

    int demuxer_started;
    int decoder_started;
    int filterer_started;
    int control_started;

    AVThreadMessageQueue *src_queue;        // user     <-> demuxer
    AVThreadMessageQueue *pkt_queue;        // demuxer  <-> decoder
    AVThreadMessageQueue *frames_queue;     // decoder  <-> filterer
    AVThreadMessageQueue *sink_queue;       // filterer <-> user

    AVThreadMessageQueue *ctl_in_queue;
    AVThreadMessageQueue *ctl_out_queue;

    int thread_stack_size;

    int64_t request_seek;

    struct info_message info;
    int has_info;

    int modules_initialized;

    int need_sync;

    int playing;
};

/* Send a message to the control input and fetch from the output until we get
 * it back */
static int send_wait_ctl_message(struct async_context *actx,
                                 struct message *msg)
{
    const int message_type = msg->type;
    const char *msg_type_str = sxpi_async_get_msg_type_string(message_type);
    TRACE(actx, "--> send %s", msg_type_str);
    int ret = av_thread_message_queue_send(actx->ctl_in_queue, msg, 0);
    if (ret < 0) {
        TRACE(actx, "couldn't send %s: %s", msg_type_str, av_err2str(ret));
        return ret;
    }
    TRACE(actx, "wait %s", msg_type_str);
    memset(msg, 0, sizeof(*msg));
    for (;;) {
        ret = av_thread_message_queue_recv(actx->ctl_out_queue, msg, 0);
        if (ret < 0 || msg->type == message_type)
            break;
        sxpi_msg_free_data(msg);
    }
    if (ret < 0)
        TRACE(actx, "couldn't get %s: %s", msg_type_str, av_err2str(ret));
    else
        TRACE(actx, "got %s", msg_type_str);
    return ret;
}

/* There might be some actions still processing in the control thread, so we
 * send a sync message to make sure every actions has been processed */
static int sync_control_thread(struct async_context *actx)
{
    if (actx->need_sync) {
        TRACE(actx, "need sync");
        struct message sync_msg = { .type = MSG_SYNC };
        int ret = send_wait_ctl_message(actx, &sync_msg);
        if (ret < 0)
            return ret;
        av_assert0(!sync_msg.data);
        actx->need_sync = 0;
    } else {
        TRACE(actx, "no need to sync");
    }
    return 0;
}

static int fetch_mod_info(struct async_context *actx)
{
    int ret;
    struct message msg = { .type = MSG_INFO };

    TRACE(actx, "fetch module info");
    if (actx->has_info)
        return 0;

    ret = sync_control_thread(actx);
    if (ret < 0)
        return ret;

    ret = send_wait_ctl_message(actx, &msg);
    if (ret < 0)
        return ret;
    av_assert0(msg.type == MSG_INFO);
    memcpy(&actx->info, msg.data, sizeof(actx->info));
    TRACE(actx, "info fetched: %dx%d duration=%s",
          actx->info.width, actx->info.height,
          PTS2TIMESTR(actx->info.duration));
    sxpi_msg_free_data(&msg);
    actx->has_info = 1;
    return 0;
}

struct async_context *sxpi_async_alloc_context(void)
{
    struct async_context *actx = av_mallocz(sizeof(*actx));
    if (!actx)
        return NULL;
    return actx;
}

int sxpi_async_fetch_info(struct async_context *actx, struct sxplayer_info *info)
{
    int ret = fetch_mod_info(actx);
    if (ret < 0)
        return ret;
    info->width    = actx->info.width;
    info->height   = actx->info.height;
    info->duration = actx->info.duration * av_q2d(AV_TIME_BASE_Q);
    info->is_image = actx->info.is_image;
    info->timebase[0] = actx->info.timebase.num;
    info->timebase[1] = actx->info.timebase.den;
    return 0;
}

int sxpi_async_pop_frame(struct async_context *actx, AVFrame **framep)
{
    int ret;
    struct message msg;

    *framep = NULL;

    ret = sync_control_thread(actx);
    if (ret < 0)
        return ret;

    if (!actx->playing) {
        TRACE(actx, "not playing, start modules");
        ret = sxpi_async_start(actx);
        if (ret < 0)
            return ret;
        ret = sync_control_thread(actx);
        if (ret < 0)
            return ret;
    }

    TRACE(actx, "fetching a frame from the sink");
    ret = av_thread_message_queue_recv(actx->sink_queue, &msg, 0);
    if (ret < 0) {
        TRACE(actx, "couldn't fetch frame from sink because %s", av_err2str(ret));
        av_thread_message_queue_set_err_send(actx->sink_queue, ret);
        (void)sxpi_async_stop(actx);
        return ret;
    }
    av_assert0(msg.type == MSG_FRAME);
    *framep = msg.data;
    return 0;
}

static int create_seek_msg(struct message *msg, int64_t ts)
{
    msg->type = MSG_SEEK,
    msg->data = av_malloc(sizeof(ts));
    if (!msg->data)
        return AVERROR(ENOMEM);
    *(int64_t *)msg->data = ts;
    return 0;
}

int sxpi_async_seek(struct async_context *actx, int64_t ts)
{
    TRACE(actx, "--> send seek msg @ %s", PTS2TIMESTR(ts));
    int ret;
    struct message msg;
    ret = create_seek_msg(&msg, ts);
    if (ret < 0)
        return ret;
    ret = av_thread_message_queue_send(actx->ctl_in_queue, &msg, 0);
    if (ret < 0) {
        av_thread_message_queue_set_err_recv(actx->ctl_in_queue, ret);
        av_freep(&msg.data);
        return ret;
    }
    actx->need_sync = 1;
    return 0;
}

int sxpi_async_start(struct async_context *actx)
{
    TRACE(actx, "--> send start msg");
    int ret;
    struct message msg = { .type = MSG_START };
    ret = av_thread_message_queue_send(actx->ctl_in_queue, &msg, 0);
    if (ret < 0) {
        av_thread_message_queue_set_err_recv(actx->ctl_in_queue, ret);
        return ret;
    }
    actx->need_sync = 1;
    return 0;
}

int sxpi_async_stop(struct async_context *actx)
{
    TRACE(actx, "--> send stop msg");
    int ret;
    struct message msg = { .type = MSG_STOP };
    ret = av_thread_message_queue_send(actx->ctl_in_queue, &msg, 0);
    if (ret < 0) {
        av_thread_message_queue_set_err_recv(actx->ctl_in_queue, ret);
        return ret;
    }
    actx->need_sync = 1;
    return 0;
}

static int initialize_modules_once(struct async_context *actx,
                                   const struct sxplayer_opts *opts)
{
    int ret;

    if (actx->modules_initialized)
        return 0;

    av_assert0(!actx->demuxer && !actx->decoder && !actx->filterer);

    TRACE(actx, "alloc modules");
    actx->demuxer  = sxpi_demuxing_alloc();
    actx->decoder  = sxpi_decoding_alloc();
    actx->filterer = sxpi_filtering_alloc();
    if (!actx->demuxer || !actx->decoder || !actx->filterer)
        return AVERROR(ENOMEM);

    TRACE(actx, "initialize modules");

    if ((ret = sxpi_demuxing_init(actx->log_ctx,
                                  actx->demuxer,
                                  actx->src_queue, actx->pkt_queue,
                                  actx->filename, opts)) < 0 ||
        (ret = sxpi_decoding_init(actx->log_ctx,
                                  actx->decoder,
                                  actx->pkt_queue, actx->frames_queue,
                                  sxpi_demuxing_get_stream(actx->demuxer), opts)) < 0 ||
        (ret = sxpi_filtering_init(actx->log_ctx,
                                   actx->filterer,
                                   actx->frames_queue, actx->sink_queue,
                                   sxpi_demuxing_get_stream(actx->demuxer),
                                   sxpi_decoding_get_avctx(actx->decoder),
                                   sxpi_demuxing_probe_rotation(actx->demuxer), opts)) < 0)
        return ret;

    actx->modules_initialized = 1;
    return 0;
}

static int alloc_msg_queue(AVThreadMessageQueue **q, int n)
{
    int ret = av_thread_message_queue_alloc(q, n, sizeof(struct message));
    if (ret < 0)
        return ret;
    av_thread_message_queue_set_free_func(*q, sxpi_msg_free_data);
    return 0;
}

#define MODULE_THREAD_FUNC(name, action)                                        \
static void *name##_thread(void *arg)                                           \
{                                                                               \
    struct async_context *actx = arg;                                           \
    sxpi_set_thread_name("sxp/" AV_STRINGIFY(name));                            \
    TRACE(actx, "[>] " AV_STRINGIFY(action) " thread starting");                \
    sxpi_##action##_run(actx->name);                                            \
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
                TRACE(actx, "stack size before: %d", (int)stack_size);          \
                pthread_attr_setstacksize(&attr, actx->thread_stack_size);      \
                stack_size = 0;                                                 \
                pthread_attr_getstacksize(&attr, &stack_size);                  \
                TRACE(actx, "stack size after: %d", (int)stack_size);           \
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

static int op_start(struct async_context *actx)
{
    struct message msg;
    int64_t seek_to = AV_NOPTS_VALUE;
    const struct sxplayer_opts *o = actx->o;

    TRACE(actx, "exec");

    int ret = initialize_modules_once(actx, o);
    if (ret < 0) {
        LOG(actx, ERROR, "initializing modules failed with %s", av_err2str(ret));
        return ret;
    }

    if (actx->request_seek != AV_NOPTS_VALUE) {
        TRACE(actx, "request seek is set to %s", PTS2TIMESTR(actx->request_seek));
        seek_to = actx->request_seek;
    } else if (o->skip64) {
        TRACE(actx, "skip is set to %s", PTS2TIMESTR(actx->o->skip64));
        seek_to = o->skip64;
    }

    if (seek_to != AV_NOPTS_VALUE) {
        TRACE(actx, "seek to: %s", PTS2TIMESTR(seek_to));

        int ret = create_seek_msg(&msg, seek_to);
        if (ret < 0)
            return ret;

        // Queue a seek request which we will pull out after the demuxer is started
        ret = av_thread_message_queue_send(actx->src_queue, &msg, 0);
        if (ret < 0) {
            LOG(actx, ERROR, "Unable to queue a seek message to the demuxer, shouldn't happen!");
            av_thread_message_queue_set_err_recv(actx->src_queue, ret);
            sxpi_msg_free_data(&msg);
            return ret;
        }

        actx->request_seek = AV_NOPTS_VALUE;
    }

    START_MODULE_THREAD(demuxer);
    START_MODULE_THREAD(decoder);
    START_MODULE_THREAD(filterer);
    if (!actx->demuxer_started ||
        !actx->decoder_started ||
        !actx->filterer_started)
        return AVERROR(ENOMEM);

    actx->playing = 1;

    if (seek_to != AV_NOPTS_VALUE) {
        TRACE(actx, "wait for seek (to %s) to come back", PTS2TIMESTR(seek_to));
        memset(&msg, 0, sizeof(msg));
        do {
            ret = av_thread_message_queue_recv(actx->sink_queue, &msg, 0);
            if (ret < 0) {
                av_thread_message_queue_set_err_send(actx->sink_queue, ret);
                return ret;
            }
            sxpi_msg_free_data(&msg);
        } while (msg.type != MSG_SEEK);
    }

    return 0;
}

static int op_info(struct async_context *actx, struct message *msg)
{
    const struct sxplayer_opts *o = actx->o;

    // We need the demuxer to be initialized to be able to call demuxing_*()
    int ret = initialize_modules_once(actx, o);
    if (ret < 0) {
        LOG(actx, ERROR, "initializing modules failed with %s", av_err2str(ret));
        return ret;
    }

    int64_t duration = o->trim_duration64 >= 0 ? o->skip64 + o->trim_duration64
                                               : AV_NOPTS_VALUE;
    const int64_t probe_duration = sxpi_demuxing_probe_duration(actx->demuxer);

    av_assert0(AV_NOPTS_VALUE < 0);
    if (probe_duration != AV_NOPTS_VALUE && (duration <= 0 ||
                                             probe_duration < duration)) {
        LOG(actx, INFO, "fix trim_duration from %f to %f",
            duration       * av_q2d(AV_TIME_BASE_Q),
            probe_duration * av_q2d(AV_TIME_BASE_Q));
        duration = probe_duration;
    }
    if (duration == AV_NOPTS_VALUE)
        duration = 0;
    const AVStream *st = sxpi_demuxing_get_stream(actx->demuxer);
    const int is_image = sxpi_demuxing_is_image(actx->demuxer);
    struct info_message info = {
        .width    = st->codecpar->width,
        .height   = st->codecpar->height,
        .duration = duration,
        .is_image = is_image,
        .timebase = st->time_base,
    };

    if (!info.timebase.num || !info.timebase.den) {
        LOG(actx, WARNING, "Invalid timebase %d/%d, assuming 1/1",
            info.timebase.num, info.timebase.den);
        info.timebase = av_make_q(1, 1);
    }

    msg->data = av_memdup(&info, sizeof(info));
    if (!msg->data)
        return AVERROR(ENOMEM);

    return 0;
}

static void kill_join_reset_workers(struct async_context *actx)
{
    TRACE(actx, "prevent modules from feeding and reading from the queues");
    av_thread_message_queue_set_err_send(actx->src_queue,    AVERROR_EXIT);
    av_thread_message_queue_set_err_send(actx->pkt_queue,    AVERROR_EXIT);
    av_thread_message_queue_set_err_send(actx->frames_queue, AVERROR_EXIT);
    av_thread_message_queue_set_err_send(actx->sink_queue,   AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->src_queue,    AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->pkt_queue,    AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->frames_queue, AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->sink_queue,   AVERROR_EXIT);

    // they won't fill the queues anymore, so we can empty them
    av_thread_message_flush(actx->src_queue);
    av_thread_message_flush(actx->pkt_queue);
    av_thread_message_flush(actx->frames_queue);
    av_thread_message_flush(actx->sink_queue);

    // now that we are sure the threads modules will stop by themselves, we can
    // join them
    TRACE(actx, "waiting for modules to end");
    JOIN_MODULE_THREAD(filterer);
    JOIN_MODULE_THREAD(decoder);
    JOIN_MODULE_THREAD(demuxer);

    // every worker ended, reset queues states
    av_thread_message_queue_set_err_send(actx->src_queue,    0);
    av_thread_message_queue_set_err_send(actx->pkt_queue,    0);
    av_thread_message_queue_set_err_send(actx->frames_queue, 0);
    av_thread_message_queue_set_err_send(actx->sink_queue,   0);
    av_thread_message_queue_set_err_recv(actx->src_queue,    0);
    av_thread_message_queue_set_err_recv(actx->pkt_queue,    0);
    av_thread_message_queue_set_err_recv(actx->frames_queue, 0);
    av_thread_message_queue_set_err_recv(actx->sink_queue,   0);
}

/* Forward the message to the modules if they are running, otherwise memorize
 * it for next time we start them */
static int op_seek(struct async_context *actx, struct message *seek_msg)
{
    int ret;
    const struct sxplayer_opts *o = actx->o;

    TRACE(actx, "exec");

    // We need the demuxer to be initialized to be able to call demuxing_*()
    ret = initialize_modules_once(actx, o);
    if (ret < 0) {
        LOG(actx, ERROR, "initializing modules failed with %s", av_err2str(ret));
        sxpi_msg_free_data(seek_msg);
        return ret;
    }

    const int64_t probe_duration = sxpi_demuxing_probe_duration(actx->demuxer);
    if (probe_duration == AV_NOPTS_VALUE) {
        TRACE(actx, "media has no duration, ignore seek");
        sxpi_msg_free_data(seek_msg);
        return 0;
    }

    actx->request_seek = *(int64_t *)seek_msg->data;

    if (!actx->playing) {
        sxpi_msg_free_data(seek_msg);
        return 0;
    }

    ret = av_thread_message_queue_send(actx->src_queue, seek_msg, 0);
    if (ret < 0) {
        /* If this errors out, it means the modules ended by themselves (no
         * stop requested by the user), so we delay the seek, reset the workers
         * and start them again */
        sxpi_msg_free_data(seek_msg);
        kill_join_reset_workers(actx);
        return op_start(actx);
    }

    // We were able to send a seek request, now we wait for it to return
    memset(seek_msg, 0, sizeof(*seek_msg));
    for (;;) {
        TRACE(actx, "seek request sent, wait for its return");
        ret = av_thread_message_queue_recv(actx->sink_queue, seek_msg, 0);
        if (ret < 0) {
            TRACE(actx, "unable to get request seek back");
            kill_join_reset_workers(actx);
            return op_start(actx);
        }
        sxpi_msg_free_data(seek_msg);
        if (seek_msg->type == MSG_SEEK)
            break;
    }

    return 0;
}

static void op_stop(struct async_context *actx)
{
    TRACE(actx, "exec");

    kill_join_reset_workers(actx);

    sxpi_demuxing_free(&actx->demuxer);
    sxpi_decoding_free(&actx->decoder);
    sxpi_filtering_free(&actx->filterer);

    actx->modules_initialized = 0;
    actx->playing = 0;
    actx->request_seek = AV_NOPTS_VALUE;
}

static void *control_thread(void *arg)
{
    int ret = 0;
    struct async_context *actx = arg;

    LOG(actx, INFO, "starting");

    sxpi_set_thread_name("sxp/control");

    for (;;) {
        enum msg_type type;
        struct message msg;

        ret = av_thread_message_queue_recv(actx->ctl_in_queue, &msg, 0);
        if (ret < 0) {
            if (ret != AVERROR_EXIT) {
                LOG(actx, ERROR, "Unable to pull a message "
                    "from the async queue: %s", av_err2str(ret));
                continue;
            }
            break;
        }
        type = msg.type;

        TRACE(actx, "--- handling OP %s", sxpi_async_get_msg_type_string(type));

        switch (type) {
        case MSG_SEEK:
            ret = op_seek(actx, &msg);
            break;
        case MSG_START:
            // XXX: fetch info first?
            if (!actx->playing)
                ret = op_start(actx);
            break;
        case MSG_STOP:
            if (actx->playing)
                op_stop(actx);
            break;
        case MSG_INFO:
            ret = op_info(actx, &msg);
            break;
        case MSG_SYNC:
            break;
        default:
            av_assert0(0);
        }

        TRACE(actx, "<-- OP %s processed", sxpi_async_get_msg_type_string(type));

        if (ret < 0) {
            LOG(actx, ERROR, "Unable to honor %s message: %s",
                sxpi_async_get_msg_type_string(type), av_err2str(ret));
            sxpi_msg_free_data(&msg);
            break;
        }

        // Forward the message to the out queue now that it has been processed
        // if it's a sync OP
        if (type == MSG_INFO || type == MSG_SYNC) {
            TRACE(actx, "forward %s to control out queue",
                  sxpi_async_get_msg_type_string(type));
            ret = av_thread_message_queue_send(actx->ctl_out_queue, &msg, 0);
            if (ret < 0) {
                // shouldn't happen
                LOG(actx, ERROR, "Unable to forward %s message to the output async queue: %s",
                    sxpi_async_get_msg_type_string(type), av_err2str(ret));
                sxpi_msg_free_data(&msg);
            }
        }
    }

    if (ret < 0) {
        av_thread_message_queue_set_err_send(actx->ctl_in_queue, ret);
        av_thread_message_queue_set_err_recv(actx->ctl_out_queue, ret);
    }
    TRACE(actx, "control thread ending");
    op_stop(actx);

    return NULL;
}

int sxpi_async_init(struct async_context *actx, void *log_ctx,
               const char *filename, const struct sxplayer_opts *o)
{
    int ret;

    av_assert0(!actx->control_started);

    actx->log_ctx = log_ctx;
    actx->filename = filename;
    actx->o = o;
    actx->thread_stack_size = o->thread_stack_size;
    actx->request_seek = AV_NOPTS_VALUE;

    TRACE(actx, "alloc modules queues");
    if ((ret = alloc_msg_queue(&actx->src_queue,    1))                 < 0 ||
        (ret = alloc_msg_queue(&actx->pkt_queue,    o->max_nb_packets)) < 0 ||
        (ret = alloc_msg_queue(&actx->frames_queue, o->max_nb_frames))  < 0 ||
        (ret = alloc_msg_queue(&actx->sink_queue,   o->max_nb_sink))    < 0)
        return ret;

    TRACE(actx, "allocate async queues");
    if ((ret = alloc_msg_queue(&actx->ctl_in_queue,  5)) < 0 ||
        (ret = alloc_msg_queue(&actx->ctl_out_queue, 5)) < 0)
        return ret;

    START_MODULE_THREAD(control);
    if (!actx->control_started)
        return AVERROR(ENOMEM); // XXX

    return 0;
}

const char *sxpi_async_get_msg_type_string(enum msg_type type)
{
    static const char * const s[NB_MSG] = {
        [MSG_FRAME]  = "frame",
        [MSG_PACKET] = "packet",
        [MSG_SEEK]   = "seek",
        [MSG_INFO]   = "info",
        [MSG_START]  = "start",
        [MSG_STOP]   = "stop",
        [MSG_SYNC]   = "sync",
    };
    return s[type];
}

static void control_quit(struct async_context *actx)
{
    sxpi_async_stop(actx);
    sync_control_thread(actx);
    av_thread_message_queue_set_err_send(actx->ctl_in_queue,  AVERROR_EXIT);
    av_thread_message_queue_set_err_send(actx->ctl_out_queue, AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->ctl_in_queue,  AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(actx->ctl_out_queue, AVERROR_EXIT);
    av_thread_message_flush(actx->ctl_in_queue);
    av_thread_message_flush(actx->ctl_out_queue);
    JOIN_MODULE_THREAD(control);
}

int sxpi_sxpi_async_started(struct async_context *actx)
{
    int ret = sync_control_thread(actx);
    if (ret < 0)
        return ret;
    return actx->playing;
}

void sxpi_async_free(struct async_context **actxp)
{
    struct async_context *actx = *actxp;

    if (!actx)
        return;

    control_quit(actx);

    av_thread_message_queue_free(&actx->src_queue);
    av_thread_message_queue_free(&actx->pkt_queue);
    av_thread_message_queue_free(&actx->frames_queue);
    av_thread_message_queue_free(&actx->sink_queue);

    av_thread_message_queue_free(&actx->ctl_in_queue);
    av_thread_message_queue_free(&actx->ctl_out_queue);

    TRACE(actx, "free done");

    av_freep(actxp);
}
