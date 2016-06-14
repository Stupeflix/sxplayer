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

#include <stdint.h>
#include <float.h> /* for DBL_MAX */

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/motion_vector.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>

#include "sxplayer.h"
#include "internal.h"

#define OFFSET(x) offsetof(struct sxplayer_ctx, x)
static const AVOption sxplayer_options[] = {
    { "avselect",               NULL, OFFSET(avselect),               AV_OPT_TYPE_INT,       {.i64=SXPLAYER_SELECT_VIDEO}, 0, NB_SXPLAYER_MEDIA_SELECTION-1 },
    { "skip",                   NULL, OFFSET(skip),                   AV_OPT_TYPE_DOUBLE,    {.dbl= 0},      0, DBL_MAX },
    { "trim_duration",          NULL, OFFSET(trim_duration),          AV_OPT_TYPE_DOUBLE,    {.dbl=-1},     -1, DBL_MAX },
    { "dist_time_seek_trigger", NULL, OFFSET(dist_time_seek_trigger), AV_OPT_TYPE_DOUBLE,    {.dbl=1.5},    -1, DBL_MAX },
    { "max_nb_packets",         NULL, OFFSET(max_nb_packets),         AV_OPT_TYPE_INT,       {.i64=5},       1, 100 },
    { "max_nb_frames",          NULL, OFFSET(max_nb_frames),          AV_OPT_TYPE_INT,       {.i64=2},       1, 100 },
    { "max_nb_sink",            NULL, OFFSET(max_nb_sink),            AV_OPT_TYPE_INT,       {.i64=2},       1, 100 },
    { "filters",                NULL, OFFSET(filters),                AV_OPT_TYPE_STRING,    {.str=NULL},    0,       0 },
    { "sw_pix_fmt",             NULL, OFFSET(sw_pix_fmt),             AV_OPT_TYPE_INT,       {.i64=SXPLAYER_PIXFMT_BGRA},  0, 1 },
    { "autorotate",             NULL, OFFSET(autorotate),             AV_OPT_TYPE_INT,       {.i64=0},       0, 1 },
    { "auto_hwaccel",           NULL, OFFSET(auto_hwaccel),           AV_OPT_TYPE_INT,       {.i64=1},       0, 1 },
    { "export_mvs",             NULL, OFFSET(export_mvs),             AV_OPT_TYPE_INT,       {.i64=0},       0, 1 },
    { "pkt_skip_mod",           NULL, OFFSET(pkt_skip_mod),           AV_OPT_TYPE_INT,       {.i64=0},       0, INT_MAX },
    { "thread_stack_size",      NULL, OFFSET(thread_stack_size),      AV_OPT_TYPE_INT,       {.i64=0},       0, INT_MAX },
    { "opaque",                 NULL, OFFSET(opaque),                 AV_OPT_TYPE_BINARY,    {.str=NULL},    0, UINT64_MAX },
    { "max_pixels",             NULL, OFFSET(max_pixels),             AV_OPT_TYPE_INT,       {.i64=0},       0, INT_MAX },
    { "audio_texture",          NULL, OFFSET(audio_texture),          AV_OPT_TYPE_INT,       {.i64=1},       0, 1 },
    { NULL }
};

static const char *sxplayer_item_name(void *arg)
{
    const struct sxplayer_ctx *s = arg;
    return s->logname;
}

static const AVClass sxplayer_class = {
    .class_name = "sxplayer",
    .item_name  = sxplayer_item_name,
    .option     = sxplayer_options,
};

int sxplayer_set_option(struct sxplayer_ctx *s, const char *key, ...)
{
    va_list ap;
    int n, ret = 0;
    double d;
    char *str;
    void *ptr;
    const AVOption *o = av_opt_find(s, key, NULL, 0, 0);

    va_start(ap, key);

    if (!o) {
        LOG(s, ERROR, "Option '%s' not found", key);
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (s->context_configured) {
        LOG(s, ERROR, "Context is already configured, can not set option '%s'", key);
        ret = AVERROR(EINVAL);
        goto end;
    }

    switch (o->type) {
    case AV_OPT_TYPE_INT:
        n = va_arg(ap, int);
        ret = av_opt_set_int(s, key, n, 0);
        break;
    case AV_OPT_TYPE_DOUBLE:
        d = va_arg(ap, double);
        ret = av_opt_set_double(s, key, d, 0);
        break;
    case AV_OPT_TYPE_STRING:
        str = va_arg(ap, char *);
        ret = av_opt_set(s, key, str, 0);
        break;
    case AV_OPT_TYPE_BINARY:
        ptr = va_arg(ap, void *);
        ret = av_opt_set_bin(s, key, ptr, sizeof(ptr), 0);
        break;
    default:
        av_assert0(0);
    }

end:
    va_end(ap);
    return ret;
}

static void free_context(struct sxplayer_ctx *s)
{
    if (!s)
        return;
    av_freep(&s->filename);
    av_freep(&s->logname);
    log_free(&s->log_ctx);
    av_opt_free(s);
    av_freep(&s);
}

/* Destroy data allocated by configure_context() */
static void free_temp_context_data(struct sxplayer_ctx *s)
{
    TRACE(s, "free temporary context data");

    av_frame_free(&s->cached_frame);

    async_free(&s->actx);

    s->context_configured = 0;
}

void sxplayer_set_log_callback(struct sxplayer_ctx *s, void *arg,
                               void (*callback)(void *arg, int level, const char *fmt, va_list vl))
{
    log_set_callback(s->log_ctx, arg, callback);
}

struct sxplayer_ctx *sxplayer_create(const char *filename)
{
    int i;
    struct sxplayer_ctx *s;
    const struct {
        const char *libname;
        unsigned build_version;
        unsigned runtime_version;
    } fflibs[] = {
        {"avutil",     LIBAVUTIL_VERSION_INT,     avutil_version()},
        {"avcodec",    LIBAVCODEC_VERSION_INT,    avcodec_version()},
        {"avformat",   LIBAVFORMAT_VERSION_INT,   avformat_version()},
        {"avfilter",   LIBAVFILTER_VERSION_INT,   avfilter_version()},
    };

    s = av_mallocz(sizeof(*s));
    if (!s)
        return NULL;

    s->filename = av_strdup(filename);
    s->logname  = av_asprintf("sxplayer:%s", av_basename(filename));
    if (!s->filename || !s->logname)
        goto fail;

    s->class = &sxplayer_class;

    av_log_set_level(LOG_LEVEL);

    s->log_ctx = log_alloc();
    if (!s->log_ctx || log_init(s->log_ctx, s) < 0)
        goto fail;

#define VFMT(v) (v)>>16, (v)>>8 & 0xff, (v) & 0xff
    for (i = 0; i < FF_ARRAY_ELEMS(fflibs); i++) {
        const unsigned bversion = fflibs[i].build_version;
        const unsigned rversion = fflibs[i].runtime_version;
        LOG(s, INFO, "lib%-12s build:%3d.%3d.%3d runtime:%3d.%3d.%3d",
            fflibs[i].libname, VFMT(bversion), VFMT(rversion));
        if (bversion != rversion)
            LOG(s, WARNING, "/!\\ build and runtime version of FFmpeg mismatch /!\\");
    }

    av_register_all();
    avfilter_register_all();

    av_opt_set_defaults(s);

    s->last_ts              = AV_NOPTS_VALUE;
    s->first_ts             = AV_NOPTS_VALUE;
    s->last_frame_poped_ts  = AV_NOPTS_VALUE;
    s->last_pushed_frame_ts = AV_NOPTS_VALUE;
    s->trim_duration64      = AV_NOPTS_VALUE;

    av_assert0(!s->context_configured);
    return s;

fail:
    free_context(s);
    return NULL;
}

void sxplayer_free(struct sxplayer_ctx **ss)
{
    struct sxplayer_ctx *s = *ss;

    if (!s)
        return;

    LOG(s, DEBUG, "destroying context");

    async_stop(s->actx);

    free_temp_context_data(s);
    free_context(s);
    *ss = NULL;
}

/**
 * Map the timeline time to the media time
 */
static int64_t get_media_time(const struct sxplayer_ctx *s, int64_t t)
{
    if (!s->trim_duration64)
        return 0;
    return s->skip64 + FFMIN(t, s->trim_duration64);
}

static int set_context_fields(struct sxplayer_ctx *s)
{
    int ret;

    if (pix_fmts_sx2ff(s->sw_pix_fmt) == AV_PIX_FMT_NONE) {
        LOG(s, ERROR, "Invalid software decoding pixel format specified");
        return AVERROR(EINVAL);
    }

    if (s->auto_hwaccel && (s->filters || s->autorotate || s->export_mvs)) {
        LOG(s, WARNING, "Filters ('%s'), autorotate (%d), or export_mvs (%d) settings "
            "are set but hwaccel is enabled, disabling auto_hwaccel so these "
            "options are honored", s->filters, s->autorotate, s->export_mvs);
        s->auto_hwaccel = 0;
    }

    LOG(s, INFO, "avselect:%d skip:%f trim_duration:%f "
        "dist_time_seek_trigger:%f queues:[%d %d %d] filters:'%s'",
        s->avselect, s->skip, s->trim_duration,
        s->dist_time_seek_trigger,
        s->max_nb_packets, s->max_nb_frames, s->max_nb_sink,
        s->filters ? s->filters : "");

    s->skip64 = TIME2INT64(s->skip);
    s->dist_time_seek_trigger64 = TIME2INT64(s->dist_time_seek_trigger);
    s->trim_duration64 = s->trim_duration < 0 ? AV_NOPTS_VALUE : TIME2INT64(s->trim_duration);

    TRACE(s, "rescaled values: skip=%s dist:%s dur:%s",
          PTS2TIMESTR(s->skip64),
          PTS2TIMESTR(s->dist_time_seek_trigger64),
          PTS2TIMESTR(s->trim_duration64));

    av_assert0(!s->actx);
    s->actx = async_alloc_context();
    if (!s->actx)
        return AVERROR(ENOMEM);

    ret = async_init(s->actx, s);
    if (ret < 0)
        return ret;

    if (s->skip64) {
        TRACE(s, "request initial skip");
        ret = async_seek(s->actx, s->skip64);
        if (ret < 0)
            return ret;
    }

    s->context_configured = 1;

    return 0;
}

/**
 * This can not be done earlier inside the context allocation function because
 * it requires user option to be set, which is done between the context
 * allocation and the first call to sxplayer_get_*frame() or sxplayer_get_duration().
 */
static int configure_context(struct sxplayer_ctx *s)
{
    int ret;

    s->last_op_was_prefetch = 0;

    if (s->context_configured)
        return 1;

    TRACE(s, "set context fields");
    ret = set_context_fields(s);
    if (ret < 0) {
        LOG(s, ERROR, "Unable to set context fields: %s", av_err2str(ret));
        free_temp_context_data(s);
        return ret;
    }

    return 0;
}

/* Return the frame only if different from previous one. We do not make a
 * simple pointer check because of the frame reference counting (and thus
 * pointer reuse, depending on many parameters)  */
static struct sxplayer_frame *ret_frame(struct sxplayer_ctx *s, AVFrame *frame)
{
    struct sxplayer_frame *ret;
    AVFrameSideData *sd;
    const float exect = LOG_LEVEL >= AV_LOG_WARNING ? (av_gettime() - s->entering_time) / 1000000. : -1;

    if (exect > 1/60.)
        LOG(s, WARNING, "getting the frame took %fs!", exect);

    if (!frame) {
        LOG(s, DEBUG, " <<< return nothing in %fs", exect);
        return NULL;
    }

    const int64_t frame_ts = frame->pts;

    TRACE(s, "last_pushed_frame_ts:%s frame_ts:%s",
          PTS2TIMESTR(s->last_pushed_frame_ts), PTS2TIMESTR(frame_ts));

    /* if same frame as previously, do not raise it again */
    if (s->last_pushed_frame_ts == frame_ts) {
        LOG(s, DEBUG, " <<< same frame as previously, return NULL in %fs", exect);
        return NULL;
    }

    ret = av_mallocz(sizeof(*ret));
    if (!ret)
        return NULL;

    s->last_pushed_frame_ts = frame_ts;

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
    if (sd) {
        ret->mvs = av_memdup(sd->data, sd->size);
        if (!ret->mvs) {
            LOG(s, ERROR, "Unable to memdup motion vectors side data");
            av_free(ret);
            return NULL;
        }
        ret->nb_mvs = sd->size / sizeof(AVMotionVector);
        TRACE(s, "export %d motion vectors", ret->nb_mvs);
    }

    ret->internal = frame;
    ret->data = frame->data[0];
    ret->linesize = frame->linesize[0];
    ret->ts       = frame_ts * av_q2d(AV_TIME_BASE_Q);
    if (s->avselect == SXPLAYER_SELECT_VIDEO ||
        (s->avselect == SXPLAYER_SELECT_AUDIO && s->audio_texture)) {
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            ret->data = frame->data[3];
#if HAVE_MEDIACODEC_HWACCEL
        } else if (frame->format == AV_PIX_FMT_MEDIACODEC) {
            ret->data = frame->data[3];
#endif
        }
        ret->width   = frame->width;
        ret->height  = frame->height;
        ret->pix_fmt = pix_fmts_ff2sx(frame->format);
    } else {
        ret->nb_samples = frame->nb_samples;
        ret->pix_fmt = smp_fmts_ff2sx(frame->format);
    }

    LOG(s, DEBUG, " <<< return %dx%d frame @ ts=%s [max:%s] in %fs",
        frame->width, frame->height, PTS2TIMESTR(frame_ts),
        PTS2TIMESTR(s->skip64 + s->trim_duration64),
        exect);
    return ret;
}

void sxplayer_release_frame(struct sxplayer_frame *frame)
{
    if (frame) {
        AVFrame *avframe = frame->internal;
        av_frame_free(&avframe);
        av_freep(&frame->mvs);
        av_free(frame);
    }
}

int sxplayer_set_drop_ref(struct sxplayer_ctx *s, int drop)
{
    return -1; // TODO
}

static AVFrame *pop_frame(struct sxplayer_ctx *s)
{
    AVFrame *frame = NULL;

    if (s->cached_frame) {
        TRACE(s, "we have a cached frame, pop this one");
        frame = s->cached_frame;
        s->cached_frame = NULL;
    } else {
        for (;;) {
            int ret;
            struct message msg;

            TRACE(s, "querying async context");
            ret = async_pop_msg(s->actx, &msg);
            if (ret < 0) {
                frame = NULL;
                TRACE(s, "poped a message raising %s", av_err2str(ret));
                break;
            } else if (msg.type == MSG_SEEK) {
                // probably from the initial seek
                TRACE(s, "got a seek message while poping, ignoring");
                async_free_message_data(&msg);
                continue;
            } else {
                TRACE(s, "got a frame!");
                frame = msg.data;
                break;
            }
        }
    }

    if (frame) {
        const int64_t ts = frame->pts;
        TRACE(s, "poped frame with ts=%s", PTS2TIMESTR(ts));
        s->last_frame_poped_ts = ts;
    } else {
        TRACE(s, "no frame available");
        /* We save the last timestamp in order to avoid restarting the decoding
         * thread again */
        if (s->last_ts == AV_NOPTS_VALUE ||
            (s->last_frame_poped_ts != AV_NOPTS_VALUE && s->last_frame_poped_ts > s->last_ts)) {
            TRACE(s, "last timestamp is apparently %s", PTS2TIMESTR(s->last_ts));
            s->last_ts = s->last_frame_poped_ts;
        }
    }

    TRACE(s, "pop frame %p", frame);
    return frame;
}

#define SYNTH_FRAME 0

#if SYNTH_FRAME
static struct sxplayer_frame *ret_synth_frame(struct sxplayer_ctx *s, double t)
{
    AVFrame *frame = av_frame_alloc();
    const int64_t t64 = TIME2INT64(t);
    const int frame_id = lrint(t * 60);

    frame->format = AV_PIX_FMT_RGBA;
    frame->width = frame->height = 1;
    frame->pts = t64;
    av_frame_get_buffer(frame, 16);
    frame->data[0][0] = (frame_id>>8 & 0xf) * 17;
    frame->data[0][1] = (frame_id>>4 & 0xf) * 17;
    frame->data[0][2] = (frame_id    & 0xf) * 17;
    frame->data[0][3] = 0xff;
    return ret_frame(s, frame);
}
#endif

int sxplayer_seek(struct sxplayer_ctx *s, double reqt)
{
    int ret;
    int retried = -1;
    struct message msg;

    LOG(s, DEBUG, "seek requested at t=%f", reqt);
    ret = configure_context(s);
    if (ret < 0)
        return ret;

    av_frame_free(&s->cached_frame);
    s->last_pushed_frame_ts = AV_NOPTS_VALUE;

    if (s->trim_duration64 == AV_NOPTS_VALUE)
        s->trim_duration64 = async_probe_duration(s->actx);

retry:
    retried++;
    ret = async_seek(s->actx, get_media_time(s, TIME2INT64(reqt)));
    if (ret < 0)
        return ret;

    do {
        ret = async_pop_msg(s->actx, &msg);
        if (!retried && (ret == AVERROR_EOF || ret == AVERROR_EXIT))
            goto retry;
        if (ret < 0)
            return ret;
        async_free_message_data(&msg);
    } while (msg.type != MSG_SEEK);
    TRACE(s, "seek message obtained");

    async_free_message_data(&msg);
    return ret;
}

int sxplayer_prefetch(struct sxplayer_ctx *s)
{
    int ret;
    const int64_t t = av_gettime();

    LOG(s, DEBUG, "prefetch requested");

    /* We don't want prefetch to seek everytime it's called. This is to
     * typically handle if the user constantly calling prefetch before
     * requesting t=0. */
    if (s->last_op_was_prefetch) {
        LOG(s, DEBUG, "prefetch already done, noop");
        return 0;
    }

    ret = configure_context(s);
    if (ret < 0)
        return ret;

    /* If the media reached EOF, then we need to seek back to the beginning of
     * the presentation for the next playback (the initial seek is usually
     * performed at the end of configure_context(), at its first init). */
    if (ret > 0) {
        TRACE(s, "context already configured, request initial skip");
        ret = async_seek(s->actx, s->skip64);
        if (ret < 0)
            return ret;
    }

    ret = async_start(s->actx);
    s->last_op_was_prefetch = 1;
    LOG(s, DEBUG, "prefetched in %fs (ret=%s)", (av_gettime() - t) / 1000000., av_err2str(ret));
    return ret;
}

struct sxplayer_frame *sxplayer_get_frame(struct sxplayer_ctx *s, double t)
{
    int ret;
    int64_t diff;
    const int64_t t64 = TIME2INT64(t);

    if (LOG_LEVEL >= AV_LOG_WARNING)
        s->entering_time = av_gettime();

    LOG(s, DEBUG, " >>> get frame for t=%g", t);

#if SYNTH_FRAME
    return ret_synth_frame(s, t);
#endif

    ret = configure_context(s);
    if (ret < 0)
        return ret_frame(s, NULL);

    if (t < 0) {
        sxplayer_prefetch(s);
        return ret_frame(s, NULL);
    }

    /* In this case, the decoding thread wasn't ever started once but we need
     * this information at least once, so we probe [async] (which will start
     * the demuxing/decoding/filtering machinery). If we're not at our first
     * sxplayer_get_frame() call then we will not enter in this block again and
     * the next checks preventing a restart will work as expected. */
    if (s->trim_duration64 == AV_NOPTS_VALUE)
        s->trim_duration64 = async_probe_duration(s->actx);

    const int64_t vt = get_media_time(s, t64);
    TRACE(s, "t=%s -> vt=%s", PTS2TIMESTR(t64), PTS2TIMESTR(vt));

    /* If the trim duration couldn't be evaluated, it's likely an image so
     * we will assume this is the case. In the case we already pushed a
     * picture we don't want to restart the decoding thread again so we
     * return NULL. */
    if (!s->trim_duration64 && s->last_pushed_frame_ts != AV_NOPTS_VALUE) {
        TRACE(s, "no trim duration, likely picture, and frame already returned");
        return ret_frame(s, NULL);
    }

    if (s->last_ts != AV_NOPTS_VALUE && vt >= s->last_ts &&
        s->last_pushed_frame_ts == s->last_ts) {
        TRACE(s, "requested the last frame again");
        return ret_frame(s, NULL);
    }

    if (s->first_ts != AV_NOPTS_VALUE && vt <= s->first_ts &&
        s->last_pushed_frame_ts == s->first_ts) {
        TRACE(s, "requested the first frame again");
        return ret_frame(s, NULL);
    }

    AVFrame *candidate = NULL;

    /* If no frame was ever pushed, we need to pop one */
    if (s->last_pushed_frame_ts == AV_NOPTS_VALUE) {

        /* If prefetch wasn't done (async not started), and we requested a time
         * that is beyond the initial skip, we request an appropriate seek
         * before we start the decoding process in order to save one seek and
         * some decoding (a seek for the initial skip, then another one soon
         * after to reach the requested time). */
        if (!async_started(s->actx) && vt > s->skip64) {
            TRACE(s, "no prefetch, but requested time (%s) beyond initial skip (%s)",
                  PTS2TIMESTR(vt), PTS2TIMESTR(s->skip64));
            async_seek(s->actx, vt);
        }

        TRACE(s, "no frame ever pushed yet, pop a candidate");
        candidate = pop_frame(s);
        if (!candidate) {
            TRACE(s, "can not get a single frame for this media");
            return ret_frame(s, NULL);
        }

        diff = vt - candidate->pts;
        TRACE(s, "diff with candidate (t=%s): %s [%"PRId64"]",
              PTS2TIMESTR(candidate->pts), PTS2TIMESTR(diff), diff);

        /* No frame was ever pushed, but the timestamp of the first frame
         * obtained is past the requested time. This should never happen if a
         * seek happened (async module will fix the timestamp in the worst
         * case), BUT it could happen in case the first timestamp of the video
         * is actually not 0. In this case, we decide to return the frame
         * anyway. */
        if (diff < 0) {
            /* Warning: we must absolutely NOT save the timestamp of the
             * candidate if the first time requested is not actually 0 */
            if (t64 == 0)
                s->first_ts = candidate->pts;
            return ret_frame(s, candidate);
        }

    } else {
        diff = vt - s->last_pushed_frame_ts;
        TRACE(s, "diff with latest frame (t=%s) returned: %s [%"PRId64"]",
              PTS2TIMESTR(s->last_pushed_frame_ts), PTS2TIMESTR(diff), diff);
    }

    if (!diff)
        return ret_frame(s, candidate);

    /* Check if a seek is needed */
    if (diff < 0 || diff > s->dist_time_seek_trigger64) {
        int retried = -1;
        struct message msg;

        if (diff < 0)
            TRACE(s, "diff %s [%"PRId64"] < 0 request backward seek", PTS2TIMESTR(diff), diff);
        else
            TRACE(s, "diff %s > %s [%"PRId64" > %"PRId64"] request future seek",
                  PTS2TIMESTR(diff), PTS2TIMESTR(s->dist_time_seek_trigger64),
                  diff, s->dist_time_seek_trigger64);

        av_frame_free(&candidate);
        av_frame_free(&s->cached_frame);

retry:
        retried++;
        async_seek(s->actx, get_media_time(s, t64));

        TRACE(s, "seek requested, wait for it to be effective");
        do {
            ret = async_pop_msg(s->actx, &msg);
            if (!retried && (ret == AVERROR_EOF || ret == AVERROR_EXIT))
                goto retry;
            if (ret < 0)
                return ret_frame(s, NULL);
            async_free_message_data(&msg);
        } while (msg.type != MSG_SEEK);
        TRACE(s, "seek message obtained");
    }

    /* Consume frames until we get a frame as accurate as possible */
    for (;;) {
        TRACE(s, "grab another frame");
        AVFrame *next = pop_frame(s);
        if (!next || next->pts > vt) {
            av_frame_free(&s->cached_frame);
            s->cached_frame = next;
            break;
        }
        av_frame_free(&candidate);
        candidate = next;
    }

    return ret_frame(s, candidate);
}

struct sxplayer_frame *sxplayer_get_next_frame(struct sxplayer_ctx *s)
{
    int ret;
    AVFrame *frame;

    if (LOG_LEVEL >= AV_LOG_WARNING)
        s->entering_time = av_gettime();

    LOG(s, DEBUG, " >>> get next frame");

    ret = configure_context(s);
    if (ret < 0)
        return ret_frame(s, NULL);

    frame = pop_frame(s);

    /* If the media reached EOF, then we need to seek back to the beginning of
     * the presentation for the next playback (the initial seek is usually
     * performed at the end of configure_context(), at its first init). */
    if (!frame) {
        TRACE(s, "query a seek back to 0 for the next demux");
        async_seek(s->actx, s->skip64);
    }

    return ret_frame(s, frame);
}

int sxplayer_get_info(struct sxplayer_ctx *s, struct sxplayer_info *info)
{
    int ret;

    LOG(s, DEBUG, "probing information");

    ret = configure_context(s);
    if (ret < 0)
        return ret;
    ret = async_fetch_info(s->actx, info);
    if (ret < 0)
        return ret;
    TRACE(s, "media info: %dx%d %f", info->width, info->height, info->duration);
    return 0;
}

int sxplayer_get_duration(struct sxplayer_ctx *s, double *duration)
{
    int ret;
    struct sxplayer_info info;

    LOG(s, DEBUG, "getting duration");

    ret = sxplayer_get_info(s, &info);
    if (ret < 0)
        return ret;
    *duration = info.duration;
    return 0;
}
