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
#include <libavutil/rational.h>
#include <libavutil/time.h>

#include "sxplayer.h"
#include "async.h"
#include "log.h"
#include "internal.h"

struct sxplayer_ctx {
    const AVClass *class;                   // necessary for the AVOption mechanism
    struct log_ctx *log_ctx;
    char *filename;                         // input filename
    char *logname;

    struct sxplayer_opts opts;

    struct async_context *actx;
    int context_configured;

    AVFrame *cached_frame;

    AVRational st_timebase;                 // stream timebase

    /* All the following ts are expressed in st_timebase unit */
    int64_t last_pushed_frame_ts;           // ts value of the latest pushed frame (it acts as a UID)
    int64_t last_frame_poped_ts;
    int64_t first_ts;
    int64_t last_ts;

    int64_t entering_time;
    const char *cur_func_name;
};

#define OFFSET(x) offsetof(struct sxplayer_ctx, opts.x)
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
    { "vt_pix_fmt",             NULL, OFFSET(vt_pix_fmt),             AV_OPT_TYPE_STRING,    {.str="bgra"},  0, 0 },
    { "stream_idx",             NULL, OFFSET(stream_idx),             AV_OPT_TYPE_INT,       {.i64=-1},     -1, INT_MAX },
    { "use_pkt_duration",       NULL, OFFSET(use_pkt_duration),       AV_OPT_TYPE_INT,       {.i64=1},       0, 1 },
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
    sxpi_log_free(&s->log_ctx);
    av_opt_free(s);
    av_freep(&s);
}

/* Destroy data allocated by configure_context() */
static void free_temp_context_data(struct sxplayer_ctx *s)
{
    TRACE(s, "free temporary context data");

    av_frame_free(&s->cached_frame);

    sxpi_async_free(&s->actx);

    s->context_configured = 0;
}

void sxplayer_set_log_callback(struct sxplayer_ctx *s, void *arg,
                               sxplayer_log_callback_type callback)
{
    sxpi_log_set_callback(s->log_ctx, arg, callback);
}

struct sxplayer_ctx *sxplayer_create(const char *filename)
{
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

    av_assert0(AV_TIME_BASE == 1000000);

    struct sxplayer_ctx *s = av_mallocz(sizeof(*s));
    if (!s)
        return NULL;

    s->filename = av_strdup(filename);
    s->logname  = av_asprintf("sxplayer:%s", av_basename(filename));
    if (!s->filename || !s->logname)
        goto fail;

    s->class = &sxplayer_class;

    av_log_set_level(LOG_LEVEL);

    s->log_ctx = sxpi_log_alloc();
    if (!s->log_ctx || sxpi_log_init(s->log_ctx, s) < 0)
        goto fail;

    LOG(s, INFO, "libsxplayer %d.%d.%d",
        SXPLAYER_VERSION_MAJOR, SXPLAYER_VERSION_MINOR, SXPLAYER_VERSION_MICRO);

#define VFMT(v) (v)>>16, (v)>>8 & 0xff, (v) & 0xff
    for (int i = 0; i < FF_ARRAY_ELEMS(fflibs); i++) {
        const unsigned bversion = fflibs[i].build_version;
        const unsigned rversion = fflibs[i].runtime_version;
        LOG(s, INFO, "lib%-12s build:%3d.%3d.%3d runtime:%3d.%3d.%3d",
            fflibs[i].libname, VFMT(bversion), VFMT(rversion));
        if (bversion != rversion)
            LOG(s, WARNING, "/!\\ build and runtime version of FFmpeg mismatch /!\\");
    }

    avformat_network_init();

    av_opt_set_defaults(s);

    s->last_ts              = AV_NOPTS_VALUE;
    s->first_ts             = AV_NOPTS_VALUE;
    s->last_frame_poped_ts  = AV_NOPTS_VALUE;
    s->last_pushed_frame_ts = AV_NOPTS_VALUE;

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

    free_temp_context_data(s);
    free_context(s);
    *ss = NULL;
}

/**
 * Map the timeline time to the media time
 */
static int64_t get_media_time(const struct sxplayer_opts *o, int64_t t)
{
    return o->skip64 + (o->trim_duration64 == AV_NOPTS_VALUE ? t : FFMIN(t, o->trim_duration64));
}

static int set_context_fields(struct sxplayer_ctx *s)
{
    struct sxplayer_opts *o = &s->opts;

    if (sxpi_pix_fmts_sx2ff(o->sw_pix_fmt) == AV_PIX_FMT_NONE) {
        LOG(s, ERROR, "Invalid software decoding pixel format specified");
        return AVERROR(EINVAL);
    }

    if (o->auto_hwaccel && (o->filters || o->autorotate || o->export_mvs)) {
        LOG(s, WARNING, "Filters ('%s'), autorotate (%d), or export_mvs (%d) settings "
            "are set but hwaccel is enabled, disabling auto_hwaccel so these "
            "options are honored", o->filters, o->autorotate, o->export_mvs);
        o->auto_hwaccel = 0;
    }

    LOG(s, INFO, "avselect:%d skip:%f trim_duration:%f "
        "dist_time_seek_trigger:%f queues:[%d %d %d] filters:'%s'",
        o->avselect, o->skip, o->trim_duration,
        o->dist_time_seek_trigger,
        o->max_nb_packets, o->max_nb_frames, o->max_nb_sink,
        o->filters ? o->filters : "");

    o->skip64 = TIME2INT64(o->skip);
    o->dist_time_seek_trigger64 = TIME2INT64(o->dist_time_seek_trigger);
    o->trim_duration64 = o->trim_duration < 0 ? AV_NOPTS_VALUE : TIME2INT64(o->trim_duration);

    TRACE(s, "rescaled values: skip=%s dist:%s dur:%s",
          PTS2TIMESTR(o->skip64),
          PTS2TIMESTR(o->dist_time_seek_trigger64),
          PTS2TIMESTR(o->trim_duration64));

    av_assert0(!s->actx);
    s->actx = sxpi_async_alloc_context();
    if (!s->actx)
        return AVERROR(ENOMEM);

    int ret = sxpi_async_init(s->actx, s->log_ctx, s->filename, &s->opts);
    if (ret < 0)
        return ret;

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
    if (s->context_configured)
        return 1;

    TRACE(s, "set context fields");
    int ret = set_context_fields(s);
    if (ret < 0) {
        LOG(s, ERROR, "Unable to set context fields: %s", av_err2str(ret));
        free_temp_context_data(s);
        return ret;
    }

    return 0;
}

static const int col_spc_map[] = {
    [AVCOL_SPC_RGB]                = SXPLAYER_COL_SPC_RGB,
    [AVCOL_SPC_BT709]              = SXPLAYER_COL_SPC_BT709,
    [AVCOL_SPC_UNSPECIFIED]        = SXPLAYER_COL_SPC_UNSPECIFIED,
    [AVCOL_SPC_RESERVED]           = SXPLAYER_COL_SPC_RESERVED,
    [AVCOL_SPC_FCC]                = SXPLAYER_COL_SPC_FCC,
    [AVCOL_SPC_BT470BG]            = SXPLAYER_COL_SPC_BT470BG,
    [AVCOL_SPC_SMPTE170M]          = SXPLAYER_COL_SPC_SMPTE170M,
    [AVCOL_SPC_SMPTE240M]          = SXPLAYER_COL_SPC_SMPTE240M,
    [AVCOL_SPC_YCGCO]              = SXPLAYER_COL_SPC_YCGCO,
    [AVCOL_SPC_BT2020_NCL]         = SXPLAYER_COL_SPC_BT2020_NCL,
    [AVCOL_SPC_BT2020_CL]          = SXPLAYER_COL_SPC_BT2020_CL,
    [AVCOL_SPC_SMPTE2085]          = SXPLAYER_COL_SPC_SMPTE2085,
    [AVCOL_SPC_CHROMA_DERIVED_NCL] = SXPLAYER_COL_SPC_CHROMA_DERIVED_NCL,
    [AVCOL_SPC_CHROMA_DERIVED_CL]  = SXPLAYER_COL_SPC_CHROMA_DERIVED_CL,
    [AVCOL_SPC_ICTCP]              = SXPLAYER_COL_SPC_ICTCP,
};

SXPI_STATIC_ASSERT(col_spc_map, FF_ARRAY_ELEMS(col_spc_map) < 32);

static int get_sxplayer_col_spc(int avcol_spc)
{
    if (avcol_spc < 0 || avcol_spc >= FF_ARRAY_ELEMS(col_spc_map))
        return SXPLAYER_COL_SPC_UNSPECIFIED;
    return col_spc_map[avcol_spc];
}

static const int col_rng_map[] = {
    [AVCOL_RANGE_UNSPECIFIED] = SXPLAYER_COL_RNG_UNSPECIFIED,
    [AVCOL_RANGE_MPEG]        = SXPLAYER_COL_RNG_LIMITED,
    [AVCOL_RANGE_JPEG]        = SXPLAYER_COL_RNG_FULL,
};

SXPI_STATIC_ASSERT(col_rng_map, FF_ARRAY_ELEMS(col_rng_map) < 32);

static int get_sxplayer_col_rng(int avcol_rng)
{
    if (avcol_rng < 0 || avcol_rng >= FF_ARRAY_ELEMS(col_rng_map))
        return SXPLAYER_COL_RNG_UNSPECIFIED;
    return col_rng_map[avcol_rng];
}

static const int col_pri_map[] = {
    [AVCOL_PRI_RESERVED0]   = SXPLAYER_COL_PRI_RESERVED0,
    [AVCOL_PRI_BT709]       = SXPLAYER_COL_PRI_BT709,
    [AVCOL_PRI_UNSPECIFIED] = SXPLAYER_COL_PRI_UNSPECIFIED,
    [AVCOL_PRI_RESERVED]    = SXPLAYER_COL_PRI_RESERVED,
    [AVCOL_PRI_BT470M]      = SXPLAYER_COL_PRI_BT470M,
    [AVCOL_PRI_BT470BG]     = SXPLAYER_COL_PRI_BT470BG,
    [AVCOL_PRI_SMPTE170M]   = SXPLAYER_COL_PRI_SMPTE170M,
    [AVCOL_PRI_SMPTE240M]   = SXPLAYER_COL_PRI_SMPTE240M,
    [AVCOL_PRI_FILM]        = SXPLAYER_COL_PRI_FILM,
    [AVCOL_PRI_BT2020]      = SXPLAYER_COL_PRI_BT2020,
    [AVCOL_PRI_SMPTE428]    = SXPLAYER_COL_PRI_SMPTE428,
    [AVCOL_PRI_SMPTE431]    = SXPLAYER_COL_PRI_SMPTE431,
    [AVCOL_PRI_SMPTE432]    = SXPLAYER_COL_PRI_SMPTE432,
    [AVCOL_PRI_JEDEC_P22]   = SXPLAYER_COL_PRI_JEDEC_P22,
};

SXPI_STATIC_ASSERT(col_pri_map, FF_ARRAY_ELEMS(col_pri_map) < 32);

static int get_sxplayer_col_pri(int avcol_pri)
{
    if (avcol_pri < 0 || avcol_pri >= FF_ARRAY_ELEMS(col_pri_map))
        return SXPLAYER_COL_PRI_UNSPECIFIED;
    return col_pri_map[avcol_pri];
}

static const int col_trc_map[] = {
    [AVCOL_TRC_RESERVED0]    = SXPLAYER_COL_TRC_RESERVED0,
    [AVCOL_TRC_BT709]        = SXPLAYER_COL_TRC_BT709,
    [AVCOL_TRC_UNSPECIFIED]  = SXPLAYER_COL_TRC_UNSPECIFIED,
    [AVCOL_TRC_RESERVED]     = SXPLAYER_COL_TRC_RESERVED,
    [AVCOL_TRC_GAMMA22]      = SXPLAYER_COL_TRC_GAMMA22,
    [AVCOL_TRC_GAMMA28]      = SXPLAYER_COL_TRC_GAMMA28,
    [AVCOL_TRC_SMPTE170M]    = SXPLAYER_COL_TRC_SMPTE170M,
    [AVCOL_TRC_SMPTE240M]    = SXPLAYER_COL_TRC_SMPTE240M,
    [AVCOL_TRC_LINEAR]       = SXPLAYER_COL_TRC_LINEAR,
    [AVCOL_TRC_LOG]          = SXPLAYER_COL_TRC_LOG,
    [AVCOL_TRC_LOG_SQRT]     = SXPLAYER_COL_TRC_LOG_SQRT,
    [AVCOL_TRC_IEC61966_2_4] = SXPLAYER_COL_TRC_IEC61966_2_4,
    [AVCOL_TRC_BT1361_ECG]   = SXPLAYER_COL_TRC_BT1361_ECG,
    [AVCOL_TRC_IEC61966_2_1] = SXPLAYER_COL_TRC_IEC61966_2_1,
    [AVCOL_TRC_BT2020_10]    = SXPLAYER_COL_TRC_BT2020_10,
    [AVCOL_TRC_BT2020_12]    = SXPLAYER_COL_TRC_BT2020_12,
    [AVCOL_TRC_SMPTE2084]    = SXPLAYER_COL_TRC_SMPTE2084,
    [AVCOL_TRC_SMPTE428]     = SXPLAYER_COL_TRC_SMPTE428,
    [AVCOL_TRC_ARIB_STD_B67] = SXPLAYER_COL_TRC_ARIB_STD_B67,
};

SXPI_STATIC_ASSERT(col_trc_map, FF_ARRAY_ELEMS(col_trc_map) < 32);

static int get_sxplayer_col_trc(int avcol_trc)
{
    if (avcol_trc < 0 || avcol_trc >= FF_ARRAY_ELEMS(col_trc_map))
        return SXPLAYER_COL_TRC_UNSPECIFIED;
    return col_trc_map[avcol_trc];
}

#define START_FUNC_BASE(name, ...) do {                     \
    s->cur_func_name = name;                                \
    if (LOG_LEVEL >= AV_LOG_WARNING)                        \
        s->entering_time = av_gettime();                    \
    LOG(s, DEBUG, ">>> " name __VA_ARGS__);                \
} while (0)

#define START_FUNC(name)      START_FUNC_BASE(name, " requested")
#define START_FUNC_T(name, t) START_FUNC_BASE(name, " requested with t=%g", t)

#define END_FUNC(max_time_warning) do {                                                                     \
    const float exect = LOG_LEVEL >= AV_LOG_WARNING ? (av_gettime() - s->entering_time) / 1000000. : -1;    \
                                                                                                            \
    if (exect > max_time_warning)                                                                           \
        LOG(s, WARNING, "getting the frame took %fs!", exect);                                              \
                                                                                                            \
    if (LOG_LEVEL >= AV_LOG_WARNING)                                                                        \
        LOG(s, DEBUG, "<<< %s executed in %fs", s->cur_func_name, exect);                                   \
} while (0)

#define MAX_ASYNC_OP_TIME (10/1000.)
#define MAX_SYNC_OP_TIME  (1/60.)

/* Return the frame only if different from previous one. We do not make a
 * simple pointer check because of the frame reference counting (and thus
 * pointer reuse, depending on many parameters)  */
static struct sxplayer_frame *ret_frame(struct sxplayer_ctx *s, AVFrame *frame)
{
    struct sxplayer_frame *ret = NULL;
    const struct sxplayer_opts *o = &s->opts;

    if (!frame) {
        LOG(s, DEBUG, "no frame to return");
        goto end;
    }

    const int64_t frame_ts = frame->pts;

    TRACE(s, "last_pushed_frame_ts:%s (%"PRId64") frame_ts:%s (%"PRId64")",
          av_ts2timestr(s->last_pushed_frame_ts, &s->st_timebase),
          s->last_pushed_frame_ts,
          av_ts2timestr(frame_ts, &s->st_timebase),
          frame_ts);

    /* if same frame as previously, do not raise it again */
    if (s->last_pushed_frame_ts == frame_ts) {
        LOG(s, DEBUG, "same frame as previously, return NULL");
        av_frame_free(&frame);
        goto end;
    }

    ret = av_mallocz(sizeof(*ret));
    if (!ret) {
        av_frame_free(&frame);
        goto end;
    }

    s->last_pushed_frame_ts = frame_ts;

    AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
    if (sd) {
        ret->mvs = av_memdup(sd->data, sd->size);
        if (!ret->mvs) {
            LOG(s, ERROR, "Unable to memdup motion vectors side data");
            av_frame_free(&frame);
            av_freep(&ret);
            goto end;
        }
        ret->nb_mvs = sd->size / sizeof(AVMotionVector);
        TRACE(s, "export %d motion vectors", ret->nb_mvs);
    }

    ret->internal = frame;
    ret->data = frame->data[0];
    ret->linesize = frame->linesize[0];
    ret->pts      = frame_ts;
    ret->ms       = av_rescale_q(frame_ts, AV_TIME_BASE_Q, s->st_timebase);
    ret->ts       = frame_ts * av_q2d(s->st_timebase);
    ret->color_space     = get_sxplayer_col_spc(frame->colorspace);
    ret->color_range     = get_sxplayer_col_rng(frame->color_range);
    ret->color_primaries = get_sxplayer_col_pri(frame->color_primaries);
    ret->color_trc       = get_sxplayer_col_trc(frame->color_trc);
    if (o->avselect == SXPLAYER_SELECT_VIDEO) {
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX ||
            frame->format == AV_PIX_FMT_VAAPI        ||
            frame->format == AV_PIX_FMT_MEDIACODEC) {
            ret->data = frame->data[3];
        }
        ret->width   = frame->width;
        ret->height  = frame->height;
        ret->pix_fmt = sxpi_pix_fmts_ff2sx(frame->format);
        LOG(s, DEBUG, "return %dx%d video frame @ ts=%s",
            frame->width, frame->height, av_ts2timestr(frame_ts, &s->st_timebase));
    } else if (o->avselect == SXPLAYER_SELECT_AUDIO && o->audio_texture) {
        ret->width   = frame->width;
        ret->height  = frame->height;
        ret->pix_fmt = SXPLAYER_SMPFMT_FLT;
        LOG(s, DEBUG, "return %dx%d audio tex frame @ ts=%s",
            frame->width, frame->height, av_ts2timestr(frame_ts, &s->st_timebase));
    } else {
        ret->nb_samples = frame->nb_samples;
        ret->pix_fmt = sxpi_smp_fmts_ff2sx(frame->format);
        LOG(s, DEBUG, "return %d samples audio frame @ ts=%s",
            frame->nb_samples, av_ts2timestr(frame_ts, &s->st_timebase));
    }

end:
    END_FUNC(MAX_SYNC_OP_TIME);
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

        /* Stream time base is required to interpret the frame PTS */
        if (!s->st_timebase.den) {
            struct sxplayer_info info;
            int ret = sxpi_async_fetch_info(s->actx, &info);
            if (ret < 0) {
                TRACE(s, "unable to fetch info %s", av_err2str(ret));
            } else {
                s->st_timebase = av_make_q(info.timebase[0], info.timebase[1]);
                LOG(s, DEBUG, "store stream timebase %d/%d",
                    s->st_timebase.num, s->st_timebase.den);
                av_assert0(s->st_timebase.den);
            }
        }

        if (s->st_timebase.den) {
            int ret = sxpi_async_pop_frame(s->actx, &frame);
            if (ret < 0)
                TRACE(s, "poped a message raising %s", av_err2str(ret));
        }
    }

    if (frame) {
        const int64_t ts = frame->pts;
        TRACE(s, "poped frame with ts=%s (%"PRId64")", av_ts2timestr(ts, &s->st_timebase), ts);
        s->last_frame_poped_ts = ts;
    } else {
        TRACE(s, "no frame available");
        /* We save the last timestamp in order to avoid restarting the decoding
         * thread again */
        if (s->last_ts == AV_NOPTS_VALUE ||
            (s->last_frame_poped_ts != AV_NOPTS_VALUE && s->last_frame_poped_ts > s->last_ts)) {
            TRACE(s, "last timestamp is apparently %s", av_ts2timestr(s->last_ts, &s->st_timebase));
            s->last_ts = s->last_frame_poped_ts;
        }
    }

    TRACE(s, "pop frame %p", frame);
    return frame;
}

#define SYNTH_FRAME 0

#if SYNTH_FRAME
static struct sxplayer_frame *ret_synth_frame(struct sxplayer_ctx *s, int64_t t64)
{
    AVFrame *frame = av_frame_alloc();
    const int frame_id = lrint(t64 * 60 / 1000000);

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
    START_FUNC_T("SEEK", reqt);

    av_frame_free(&s->cached_frame);
    s->last_pushed_frame_ts = AV_NOPTS_VALUE;

    int ret = configure_context(s);
    if (ret < 0)
        return ret;

    const struct sxplayer_opts *o = &s->opts;
    ret = sxpi_async_seek(s->actx, get_media_time(o, TIME2INT64(reqt)));
    END_FUNC(MAX_ASYNC_OP_TIME);
    return ret;
}

int sxplayer_stop(struct sxplayer_ctx *s)
{
    START_FUNC("STOP");

    av_frame_free(&s->cached_frame);
    s->last_pushed_frame_ts = AV_NOPTS_VALUE;

    int ret = configure_context(s);
    if (ret < 0)
        return ret;

    ret = sxpi_async_stop(s->actx);
    END_FUNC(MAX_ASYNC_OP_TIME);
    return ret;
}

int sxplayer_start(struct sxplayer_ctx *s)
{
    START_FUNC("START");

    int ret = configure_context(s);
    if (ret < 0)
        return ret;

    ret = sxpi_async_start(s->actx);
    END_FUNC(MAX_ASYNC_OP_TIME);
    return ret;
}

/*
 * Stream timebase must be known when this function is called.
 */
static inline int64_t stream_time(const struct sxplayer_ctx *s, int64_t t)
{
    av_assert0(s->st_timebase.den);
    return av_rescale_q(t, AV_TIME_BASE_Q, s->st_timebase);
}

struct sxplayer_frame *sxplayer_get_frame_ms(struct sxplayer_ctx *s, int64_t t64)
{
    int64_t diff;
    const struct sxplayer_opts *o = &s->opts;

    START_FUNC_T("GET FRAME", t64 / 1000000.);

#if SYNTH_FRAME
    return ret_synth_frame(s, t64);
#endif

    int ret = configure_context(s);
    if (ret < 0)
        return ret_frame(s, NULL);

    if (t64 < 0) {
        sxplayer_start(s);
        return ret_frame(s, NULL);
    }

    const int64_t vt = get_media_time(o, t64);
    TRACE(s, "t=%s -> vt=%s", PTS2TIMESTR(t64), PTS2TIMESTR(vt));

    if (s->last_ts != AV_NOPTS_VALUE && stream_time(s, vt) >= s->last_ts &&
        s->last_pushed_frame_ts == s->last_ts) {
        TRACE(s, "requested the last frame again");
        return ret_frame(s, NULL);
    }

    if (s->first_ts != AV_NOPTS_VALUE && stream_time(s, vt) <= s->first_ts &&
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
        if (!sxpi_sxpi_async_started(s->actx) && vt > o->skip64) {
            TRACE(s, "no prefetch, but requested time (%s) beyond initial skip (%s)",
                  PTS2TIMESTR(vt), PTS2TIMESTR(o->skip64));
            sxpi_async_seek(s->actx, vt);
        }

        TRACE(s, "no frame ever pushed yet, pop a candidate");
        candidate = pop_frame(s);
        if (!candidate) {
            TRACE(s, "can not get a single frame for this media");
            return ret_frame(s, NULL);
        }

        /* At this point we can assume the stream timebase is known because
         * pop_frame() was called. */
        const int64_t stt = stream_time(s, vt);
        diff = stt - candidate->pts;

        TRACE(s, "diff with candidate (t=%s): %s [%"PRId64"]",
              av_ts2timestr(candidate->pts, &s->st_timebase),
              av_ts2timestr(diff, &s->st_timebase), diff);

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
        /* At this point we can assume the stream timebase is known because
         * a frame was already pushed. */
        const int64_t stt = stream_time(s, vt);
        diff = stt - s->last_pushed_frame_ts;

        TRACE(s, "diff with latest frame (t=%s) returned: %s [%"PRId64"]",
              av_ts2timestr(s->last_pushed_frame_ts, &s->st_timebase),
              av_ts2timestr(diff, &s->st_timebase),
              diff);
    }

    if (!diff)
        return ret_frame(s, candidate);

    /* Check if a seek is needed */
    const int forward_seek = av_compare_ts(diff, s->st_timebase, o->dist_time_seek_trigger64, AV_TIME_BASE_Q) >= 0;
    if (diff < 0 || forward_seek) {
        if (diff < 0)
            TRACE(s, "diff %s [%"PRId64"] < 0 request backward seek",
                  av_ts2timestr(diff, &s->st_timebase), diff);
        else
            TRACE(s, "diff %s > %s request future seek",
                  av_ts2timestr(diff, &s->st_timebase),
                  PTS2TIMESTR(o->dist_time_seek_trigger64));

        /* If we never returned a frame and got a candidate, we do not free it
         * immediately, because after the seek we might not actually get
         * anything. Typical case: images where we request a random timestamp.
         *
         * There is however an exception if the frame is from MediaCodec. Since
         * the MediaCodec flush command discard both input and output buffers,
         * we need to release any frame we retain before performing a seek
         * otherwise the ffmpeg MediaCodec decoder will never send the command
         * to the codec and will queue input packets until it reaches EOF.
         */
        if ((candidate && candidate->format == AV_PIX_FMT_MEDIACODEC) ||
            (diff > 0 && s->last_pushed_frame_ts != AV_NOPTS_VALUE)) {
            av_frame_free(&candidate);
        }

        av_frame_free(&s->cached_frame);

        ret = sxpi_async_seek(s->actx, vt);
        if (ret < 0) {
            av_frame_free(&candidate);
            return ret_frame(s, NULL);
        }
    }

    /* Consume frames until we get a frame as accurate as possible */
    for (;;) {
        const int next_is_cached_frame = !!s->cached_frame;

        TRACE(s, "grab another frame");
        AVFrame *next = pop_frame(s);
        av_assert0(!s->cached_frame);
        if (!next) {
            TRACE(s, "no more frame");
            break;
        }

        /* We must not use av_compare_ts() here, because it doesn't make the
         * comparison with the lowest timebase value. Instead, we need to lower
         * our timestamp precision to the stream one to get as accurate as
         * possible. */
        const int64_t rescaled_vt = stream_time(s, vt);

        if (s->opts.use_pkt_duration && next->pkt_duration > 0 && rescaled_vt >= next->pts) {
            const int64_t next_guessed_pts = next->pts + next->pkt_duration;
            if (rescaled_vt < next_guessed_pts) {
                av_frame_free(&candidate);
                av_frame_free(&s->cached_frame);
                s->cached_frame = NULL;
                return ret_frame(s, next);
            }
        }

        if (next->pts > rescaled_vt) {
            TRACE(s, "grabbed frame is in the future %s > %s",
                  av_ts2timestr(next->pts, &s->st_timebase), PTS2TIMESTR(vt));
            if (!candidate && !next_is_cached_frame && s->last_pushed_frame_ts == AV_NOPTS_VALUE) {
                candidate = next;
                TRACE(s, "we need to return a frame, select this future frame anyway");
            } else {
                s->cached_frame = next;
                TRACE(s, "cache frame %s for next call", av_ts2timestr(next->pts, &s->st_timebase));
            }
            break;
        }
        av_frame_free(&candidate);
        candidate = next;
        if (candidate->pts == rescaled_vt) {
            TRACE(s, "grabbed exact frame %s", av_ts2timestr(candidate->pts, &s->st_timebase));
            break;
        }
    }

    return ret_frame(s, candidate);
}

struct sxplayer_frame *sxplayer_get_frame(struct sxplayer_ctx *s, double t)
{
    return sxplayer_get_frame_ms(s, TIME2INT64(t));
}

struct sxplayer_frame *sxplayer_get_next_frame(struct sxplayer_ctx *s)
{
    START_FUNC("GET NEXT FRAME");

    int ret = configure_context(s);
    if (ret < 0)
        return ret_frame(s, NULL);

    AVFrame *frame = pop_frame(s);
    return ret_frame(s, frame);
}

int sxplayer_get_info(struct sxplayer_ctx *s, struct sxplayer_info *info)
{
    START_FUNC("GET INFO");

    int ret = configure_context(s);
    if (ret < 0)
        goto end;
    ret = sxpi_async_fetch_info(s->actx, info);
    if (ret < 0)
        goto end;
    TRACE(s, "media info: %dx%d %f tb:%d/%d",
          info->width, info->height, info->duration,
          info->timebase[0], info->timebase[1]);

end:
    END_FUNC(1.0);
    return ret;
}

int sxplayer_get_duration(struct sxplayer_ctx *s, double *duration)
{
    START_FUNC("GET DURATION");

    struct sxplayer_info info;
    int ret = sxplayer_get_info(s, &info);
    if (ret < 0)
        goto end;
    *duration = info.duration;

end:
    END_FUNC(1.0);
    return ret;
}
