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

#include <libavutil/avassert.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>

#include "internal.h"
#include "async.h"

struct async_decoder {
    const AVClass *class;
    struct decoder_ctx *codec_ctx;
    void *priv_data;
    push_frame_func_type push_frame_cb;
    int pkt_id_match;

    int started;
    pthread_t tid;
    pthread_t watcher_tid;

    AVThreadMessageQueue *pkt_queue;
    AVThreadMessageQueue *frames_queue;

    int max_packets_queue;
    int max_frames_queue;

    AVRational st_timebase;
    AVFrame *tmp_frame;

    int64_t seek_request;
};

struct async_reader {
    const AVClass *class;
    void *priv_data;
    pull_packet_func_type pull_packet_cb;
    struct async_decoder decoder;
    seek_func_type seek_cb;

    int started;
    pthread_t tid;

    pthread_mutex_t lock;
    int64_t request_seek;
};

struct async_context {
    const AVClass *class;
    struct async_reader reader;
};

enum msg_type {
    MSG_PACKET,
    MSG_SEEK,
};

struct message {
    void *data;
    enum msg_type type;
};

static const AVClass async_context_class = {
    .class_name = "async_context",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

struct async_context *async_alloc_context(void)
{
    struct async_context *actx = av_mallocz(sizeof(*actx));
    if (!actx)
        return NULL;
    actx->class = &async_context_class;
    return actx;
}

static const AVClass async_reader_class = {
    .class_name = "async_reader",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

static void free_packet_message(void *arg)
{
    struct message *msg = arg;
    switch (msg->type) {
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

static void free_frame_message(void *arg)
{
    AVFrame **frame = arg;
    av_frame_free(frame);
}

int async_reader_seek(struct async_reader *r, int64_t ts)
{
    int ret = AVERROR(pthread_mutex_lock(&r->lock));
    if (ret < 0)
        return ret;
    r->request_seek = ts;
    ret = AVERROR(pthread_mutex_unlock(&r->lock));
    if (ret < 0)
        return ret;
    return 0;
}

static void reset_reader(struct async_reader *r)
{
    r->started = 0;
    r->request_seek = -1.0;
}

int async_register_reader(struct async_context *actx,
                          void *priv,
                          pull_packet_func_type pull_packet_cb,
                          seek_func_type seek_cb,
                          struct async_reader **r)
{
    int ret;
    struct async_reader *reader = &actx->reader;

    ret = pthread_mutex_init(&reader->lock, NULL);
    if (ret < 0)
        return AVERROR(ret);

    reader->class = &async_reader_class;
    av_opt_set_defaults(reader);

    reader->priv_data      = priv;
    reader->pull_packet_cb = pull_packet_cb;
    reader->seek_cb        = seek_cb;

    reset_reader(reader);

    *r = reader;
    return 0;
}

#define OFFSET_DEC(x) offsetof(struct async_decoder, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM
static const AVOption async_decoder_options[] = {
    { "max_packets_queue", "set the maximum number of packets in the queue", OFFSET_DEC(max_packets_queue), AV_OPT_TYPE_INT, {.i64=5}, 1, 100, FLAGS },
    { "max_frames_queue",  "set the maximum number of frames in the queue",  OFFSET_DEC(max_frames_queue),  AV_OPT_TYPE_INT, {.i64=3}, 1, 100, FLAGS },
    { NULL }
};

static const AVClass async_decoder_class = {
    .class_name = "async_decoder",
    .item_name  = av_default_item_name,
    .option     = async_decoder_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

int async_register_decoder(struct async_reader *r,
                           struct decoder_ctx *codec_ctx, void *priv,
                           push_frame_func_type push_frame_cb,
                           struct async_decoder **d,
                           AVRational st_timebase)
{
    struct async_decoder *adec = &r->decoder;

    adec->class = &async_decoder_class;
    av_opt_set_defaults(adec);

    adec->codec_ctx     = codec_ctx;
    adec->priv_data     = priv;
    adec->push_frame_cb = push_frame_cb;
    adec->st_timebase   = st_timebase;

    *d = adec;
    return 0;
}

static int64_t get_best_effort_ts(const AVFrame *f)
{
    const int64_t t = av_frame_get_best_effort_timestamp(f);
    return t != AV_NOPTS_VALUE ? t : f->pts;
}

/* Watch the frames queue and push them to the user */
static void *watcher_thread(void *arg)
{
    int ret;
    struct async_decoder *d = arg;

    DBG("watcher", "watching thread starting\n");

    for (;;) {
        AVFrame *frame;

        /* Wait to get a frame from the queue. If it fails, then the watcher
         * has to die. */
        ret = av_thread_message_queue_recv(d->frames_queue, &frame, 0);
        if (ret < 0) {
            av_thread_message_queue_set_err_send(d->frames_queue, ret);
            break;
        }

        DBG("watcher", "pushing frame ts=%s to user\n", PTS2TIMESTR(frame->pts));

        ret = d->push_frame_cb(d->priv_data, frame);
        if (ret < 0) {
            av_thread_message_queue_set_err_send(d->frames_queue, ret);
            break;
        }
    }

    do {
        ret = d->push_frame_cb(d->priv_data, NULL);
    } while (ret == AVERROR(EAGAIN));

    DBG("watcher", "watching thread ending\n");
    return NULL;
}

static int queue_frame(struct async_decoder *d, AVFrame *frame)
{
    int ret;

    DBG("queue_frame", "queue frame with ts=%s\n", PTS2TIMESTR(frame->pts));

    if (d->seek_request != AV_NOPTS_VALUE && d->seek_request > 0 && frame->pts > d->seek_request) {
        DBG("queue_frame", "frame is after requested time, fixup its ts from %s to %s\n",
            PTS2TIMESTR(frame->pts), PTS2TIMESTR(d->seek_request));
        frame->pts = d->seek_request;
        d->seek_request = AV_NOPTS_VALUE;
    }

    ret = av_thread_message_queue_send(d->frames_queue, &frame, 0);
    if (ret < 0) {
        if (ret != AVERROR_EOF)
            av_log(d, AV_LOG_ERROR, "Unable to push frame: %s\n", av_err2str(ret));
        av_thread_message_queue_set_err_recv(d->frames_queue, ret);
    }
    return ret;
}

static int queue_cached_frame(struct async_decoder *d)
{
    const int64_t cached_ts = av_rescale_q_rnd(get_best_effort_ts(d->tmp_frame), d->st_timebase, AV_TIME_BASE_Q, 0);
    DBG("queue_frame", "got a cached frame (t=%s) to push\n", PTS2TIMESTR(cached_ts));
    AVFrame *prev_frame = d->tmp_frame;
    d->tmp_frame = NULL;
    prev_frame->pts = cached_ts;
    return queue_frame(d, prev_frame);
}

int async_queue_frame(struct async_decoder *d, AVFrame *frame)
{
    int ret;

    if (!frame) {
        DBG("queue_frame", "async_queue_frame() called for flushing\n");
        if (d->tmp_frame) {
            ret = queue_cached_frame(d);
            if (ret < 0)
                return ret;
        }
        return AVERROR_EOF;
    }

    /* Rescale the timestamp to a global large time base: AV_TIME_BASE_Q */
    const int64_t ts = av_rescale_q_rnd(get_best_effort_ts(frame), d->st_timebase, AV_TIME_BASE_Q, 0);

    DBG("queue_frame", "processing frame with ts=%s\n", PTS2TIMESTR(ts));

    if (d->seek_request != AV_NOPTS_VALUE && ts < d->seek_request) {
        DBG("queue_frame", "frame ts:%s, skipping because before %s\n",
            PTS2TIMESTR(ts), PTS2TIMESTR(d->seek_request));
        av_frame_free(&d->tmp_frame);
        d->tmp_frame = frame;
        return 0;
    }

    if (d->tmp_frame) {
        ret = queue_cached_frame(d);
        if (ret < 0)
            return ret;
    }

    frame->pts = ts;
    return queue_frame(d, frame);
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

static void *decoder_thread(void *arg)
{
    int ret;
    struct async_decoder *d = arg;

    DBG("async_decoder", "start decoder thread\n");

    ret = decoder_init(d->codec_ctx, d->priv_data);
    if (ret < 0)
        return NULL;

    /* Initialize the frame queue (communication decode <-> watcher) */
    ret = av_thread_message_queue_alloc(&d->frames_queue, d->max_frames_queue, sizeof(AVFrame *));
    if (ret < 0)
        return NULL;
    av_thread_message_queue_set_free_func(d->frames_queue, free_frame_message);

    /* Spawn frame queue watcher */
    DBG("async_decoder", "decoding thread starting\n");
    if (pthread_create(&d->watcher_tid, NULL, watcher_thread, d)) {
        ret = AVERROR(errno);
        av_log(d, AV_LOG_ERROR, "Unable to start watcher thread: %s\n",
               av_err2str(ret));
        av_thread_message_queue_free(&d->frames_queue);
        return NULL;
    }

    d->seek_request = AV_NOPTS_VALUE;

    /* Main packet decoding loop */
    DBG("async_decoder", "main packet decoding loop\n");
    for (;;) {
        struct message msg;

        ret = av_thread_message_queue_recv(d->pkt_queue, &msg, 0);
        if (ret < 0) {
            av_thread_message_queue_set_err_send(d->pkt_queue, ret);
            break;
        }

        if (msg.type == MSG_SEEK) {
            const int64_t seek_ts = *(int64_t *)msg.data;
            av_freep(&msg.data);

            DBG("async_decoder", "got a seek message (to %s) in the pkt queue\n",
                PTS2TIMESTR(seek_ts));

            /* Make sure the decoder has no packet remaining to consume and
             * pushed (or dropped) all its cached frames. After this flush, we
             * can assume that the decoder will not called async_queue_frame()
             * until a new packet is pushed. */
            decoder_flush(d->codec_ctx);

            /* Let's save some little time by dropping frames in the queue so
             * the user don't get a shit ton of false positives before the
             * frames he requested. */
            av_thread_message_flush(d->frames_queue);

            /* Mark the seek request so async_queue_frame() can do its
             * filtering work. */
            d->seek_request = seek_ts;

            continue;
        }

        ret = decoder_push_packet(d->codec_ctx, msg.data);
        free_packet_message(&msg);
        if (ret < 0)
            break;
    }

    /* flush cached frames */
    DBG("async_decoder", "flush cached frames\n");
    do {
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = NULL;
        pkt.size = 0;
        ret = decoder_push_packet(d->codec_ctx, &pkt);
    } while (ret == 0 || ret == AVERROR(EAGAIN));

    decoder_uninit(d->codec_ctx);

    /* Decoder ends, notify frame watcher so it dies */
    av_thread_message_queue_set_err_recv(d->frames_queue, ret < 0 ? ret : AVERROR_EOF);
    pthread_join(d->watcher_tid, NULL);

    av_thread_message_queue_free(&d->frames_queue);
    av_frame_free(&d->tmp_frame);

    av_thread_message_queue_set_err_send(d->pkt_queue, ret < 0 ? ret : AVERROR_EOF);

    DBG("async_decoder", "decoding thread ending\n");
    return NULL;
}

static void *reader_thread(void *arg)
{
    int ret;
    struct async_reader *r = arg;
    struct async_decoder *d = &r->decoder;

    DBG("reader_thread", "reader thread starting\n");

    /* Spawn decoder */

    /* Initialize the packet queue (communication reader <-> decoder) */
    ret = av_thread_message_queue_alloc(&d->pkt_queue, d->max_packets_queue,
                                        sizeof(struct message));
    if (ret < 0) {
        return NULL;
    }
    av_thread_message_queue_set_free_func(d->pkt_queue, free_packet_message);

    DBG("reader_thread", "spawn decoder thread\n");

    /* Start its working thread */
    if (pthread_create(&d->tid, NULL, decoder_thread, d)) {
        ret = AVERROR(errno);
        av_log(d, AV_LOG_ERROR, "Unable to start decoding thread: %s\n",
               av_err2str(ret));
        goto end;
    }

    d->started = 1;

    while (1) {
        int64_t seek_to;
        AVPacket pkt;
        struct message msg = {.type = MSG_PACKET};

        /* get seek value and reset request */
        ret = AVERROR(pthread_mutex_lock(&r->lock));
        if (ret < 0)
            break;
        seek_to = r->request_seek;
        r->request_seek = -1.0;
        ret = AVERROR(pthread_mutex_unlock(&r->lock));
        if (ret < 0)
            break;

        if (seek_to >= 0) {

            /* notify the decoder about the seek by using its pkt queue */
            DBG("reader_thread", "forward seek message (to %s) to decoder\n",
                PTS2TIMESTR(seek_to));
            ret = push_seek_message(d->pkt_queue, seek_to);
            if (ret < 0)
                break;

            /* call user seek (actual seek in the reader) so the following
             * packet that will be pulled in this current thread will be
             * at the (approximate) requested time */
            ret = r->seek_cb(r->priv_data, seek_to);
            if (ret < 0)
                break;
        }

        ret = r->pull_packet_cb(r->priv_data, &pkt);
        DBG("reader_thread", "pull_packet_cb -> %s\n", av_err2str(ret));

        if (ret == AVERROR(EAGAIN)) {
            av_usleep(10000);
            continue;
        }
        if (ret < 0)
            break;

        DBG("reader_thread", "pulled a packet of size %d, sending to decoder\n", pkt.size);

        msg.data = av_memdup(&pkt, sizeof(pkt));
        if (!msg.data) {
            av_packet_unref(&pkt);
            break;
        }

        ret = av_thread_message_queue_send(d->pkt_queue, &msg, 0);
        DBG("reader_thread", "sent packet to decoder, ret=%s\n", av_err2str(ret));

        if (ret < 0) {
            free_packet_message(&msg);
            if (ret != AVERROR_EOF)
                av_log(r, AV_LOG_ERROR, "Unable to send packet to decoder: %s\n", av_err2str(ret));
            DBG("reader_thread", "can't send pkt to decoder: %s\n", av_err2str(ret));
            av_thread_message_queue_set_err_recv(d->pkt_queue, ret);
            break;
        }
    }

end:

    DBG("reader_thread", "notify decoder about %s\n", av_err2str(ret < 0 ? ret : AVERROR_EOF));

    /* Notify the decoder about the error/EOF so it dies */
    av_thread_message_queue_set_err_recv(d->pkt_queue, ret < 0 ? ret : AVERROR_EOF);
    if (d->started) {
        DBG("reader_thread", "join decoding thread\n");
        ret = pthread_join(d->tid, NULL);
        if (ret)
            av_log(r, AV_LOG_ERROR, "Unable to join decoder: %s\n",
                   av_err2str(AVERROR(ret)));
        DBG("reader_thread", "decoding thread joined\n");
        d->started = 0;
    }
    av_thread_message_queue_free(&d->pkt_queue);

    DBG("reader_thread", "reader thread ending\n");
    return NULL;
}

int async_start(struct async_context *actx)
{
    int ret;

    DBG("async_start", "Starting Async loop\n");

    struct async_reader *r = &actx->reader;

    ret = pthread_create(&r->tid, NULL, reader_thread, r);
    if (ret) {
        const int err = AVERROR(ret);
        av_log(actx, AV_LOG_ERROR, "Unable to start reader thread: %s\n",
               av_err2str(err));
        return err;
    }
    r->started = 1;

    return 0;
}

int async_wait(struct async_context *actx)
{
    DBG("async_wait", "waiting for reader to end\n");
    struct async_reader *r = &actx->reader;

    if (r->started) {
        DBG("async_wait", "join reader thread\n");
        int ret = pthread_join(r->tid, NULL);
        if (ret)
            av_log(actx, AV_LOG_ERROR, "Unable to join reader: %s\n",
                   av_err2str(AVERROR(ret)));
        DBG("async_wait", "reader thread joined\n");
        reset_reader(r);
    }
    return 0;
}

void async_free(struct async_context **actxp)
{
    struct async_context *actx = *actxp;

    if (!actx)
        return;
    av_freep(actxp);
}
