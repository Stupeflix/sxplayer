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
#include <pthread.h>
#include <float.h> /* for DBL_MAX */

#include <libavcodec/avfft.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/display.h>
#include <libavutil/eval.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/timestamp.h>
#include <libavutil/motion_vector.h>
//#include <libswresample/swresample.h>
//#include <libswscale/swscale.h>

#include "sxplayer.h"
#include "async.h"
#include "internal.h"

extern const struct decoder decoder_ffmpeg;
static const struct decoder *decoder_def_software = &decoder_ffmpeg;

#if __APPLE__
extern const struct decoder decoder_vt;
static const struct decoder *decoder_def_hwaccel = &decoder_vt;
#else
static const struct decoder *decoder_def_hwaccel = NULL;
#endif

static const struct {
    enum AVPixelFormat ff;
    enum sxplayer_pixel_format sx;
} pix_fmts_mapping[] = {
    {AV_PIX_FMT_VIDEOTOOLBOX, SXPLAYER_PIXFMT_VT},
    {AV_PIX_FMT_BGRA,         SXPLAYER_PIXFMT_BGRA},
    {AV_PIX_FMT_RGBA,         SXPLAYER_PIXFMT_RGBA},
};

#define OFFSET(x) offsetof(struct sxplayer_ctx, x)
static const AVOption sxplayer_options[] = {
    { "avselect",               NULL, OFFSET(avselect),               AV_OPT_TYPE_INT,       {.i64=SXPLAYER_SELECT_VIDEO}, 0, NB_SXPLAYER_MEDIA_SELECTION-1 },
    { "skip",                   NULL, OFFSET(skip),                   AV_OPT_TYPE_DOUBLE,    {.dbl= 0},      0, DBL_MAX },
    { "trim_duration",          NULL, OFFSET(trim_duration),          AV_OPT_TYPE_DOUBLE,    {.dbl=-1},     -1, DBL_MAX },
    { "dist_time_seek_trigger", NULL, OFFSET(dist_time_seek_trigger), AV_OPT_TYPE_DOUBLE,    {.dbl=1.5},    -1, DBL_MAX },
    { "max_nb_packets",         NULL, OFFSET(max_nb_packets),         AV_OPT_TYPE_INT,       {.i64=5},       1, 100 },
    { "max_nb_frames",          NULL, OFFSET(max_nb_frames),          AV_OPT_TYPE_INT,       {.i64=3},       1, 100 },
    { "filters",                NULL, OFFSET(filters),                AV_OPT_TYPE_STRING,    {.str=NULL},    0,       0 },
    { "sw_pix_fmt",             NULL, OFFSET(sw_pix_fmt),             AV_OPT_TYPE_INT,       {.i64=SXPLAYER_PIXFMT_BGRA},  0, 1 },
    { "autorotate",             NULL, OFFSET(autorotate),             AV_OPT_TYPE_INT,       {.i64=0},       0, 1 },
    { "auto_hwaccel",           NULL, OFFSET(auto_hwaccel),           AV_OPT_TYPE_INT,       {.i64=1},       0, 1 },
    { "export_mvs",             NULL, OFFSET(export_mvs),             AV_OPT_TYPE_INT,       {.i64=0},       0, 1 },
    { "pkt_skip_mod",           NULL, OFFSET(pkt_skip_mod),           AV_OPT_TYPE_INT,       {.i64=0},       0, INT_MAX },
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
    const AVOption *o = av_opt_find(s, key, NULL, 0, 0);

    va_start(ap, key);

    if (!o) {
        fprintf(stderr, "Option '%s' not found\n", key);
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
    default:
        av_assert0(0);
    }

end:
    va_end(ap);
    return ret;
}

static enum AVPixelFormat pix_fmts_sx2ff(enum sxplayer_pixel_format pix_fmt)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(pix_fmts_mapping); i++)
        if (pix_fmts_mapping[i].sx == pix_fmt)
            return pix_fmts_mapping[i].ff;
    return AV_PIX_FMT_NONE;
}

static enum sxplayer_pixel_format pix_fmts_ff2sx(enum AVPixelFormat pix_fmt)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(pix_fmts_mapping); i++)
        if (pix_fmts_mapping[i].ff == pix_fmt)
            return pix_fmts_mapping[i].sx;
    return -1;
}

static AVFrame *get_audio_frame(void)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return NULL;

    frame->format = AV_PIX_FMT_RGB32;

    frame->width  = AUDIO_NBSAMPLES/2;      // samples are float (32 bits), pix fmt is rgb32 (32 bits as well)
    /* height:
     *   AUDIO_NBCHANNELS (waves lines)
     * + AUDIO_NBCHANNELS (fft lines of width AUDIO_NBCHANNELS/2, or 1<<(AUDIO_NBITS-1))
     * + AUDIO_NBITS-1 AUDIO_NBCHANNELS (fft lines downscaled) */
    frame->height = (1 + AUDIO_NBITS) * AUDIO_NBCHANNELS;

    if (av_frame_get_buffer(frame, 16) < 0) {
        av_frame_free(&frame);
        return NULL;
    }

    memset(frame->data[0], 0, frame->height * frame->linesize[0]);

    return frame;
}

static void free_context(struct sxplayer_ctx *s)
{
    if (!s)
        return;
    av_freep(&s->filename);
    av_freep(&s->logname);
    av_freep(&s);
}

/* Destroy data allocated by configure_context() */
static void free_temp_context_data(struct sxplayer_ctx *s)
{
    TRACE(s, "free temporary context data");

    av_frame_free(&s->filtered_frame);
    avfilter_graph_free(&s->filter_graph);
    av_frame_free(&s->queued_frame);
    av_frame_free(&s->cached_frame);

    if (s->media_type == AVMEDIA_TYPE_AUDIO) {
        av_frame_free(&s->audio_texture_frame);
        av_frame_free(&s->tmp_audio_frame);
        av_freep(&s->window_func_lut);
        av_freep(&s->rdft_data[0]);
        av_freep(&s->rdft_data[1]);
        if (s->rdft) {
            av_rdft_end(s->rdft);
            s->rdft = NULL;
        }
    }

    if (s->fmt_ctx) {
        decoder_free(&s->dec_ctx);
        avformat_close_input(&s->fmt_ctx);
    }
    async_free(&s->actx);

    s->context_configured = 0;
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
        //{"swscale",    LIBSWSCALE_VERSION_INT,    swscale_version()},
        {"avfilter",   LIBAVFILTER_VERSION_INT,   avfilter_version()},
        //{"swresample", LIBSWRESAMPLE_VERSION_INT, swresample_version()},
    };

    s = av_mallocz(sizeof(*s));
    if (!s)
        return NULL;

    s->filename = av_strdup(filename);
    s->logname  = av_asprintf("sxplayer:%s", av_basename(filename));
    if (!s->filename || !s->logname)
        goto fail;

    s->class = &sxplayer_class;

    if (ENABLE_INFO)
        av_log_set_level(AV_LOG_INFO);
    else
        av_log_set_level(ENABLE_DBG ? AV_LOG_TRACE : AV_LOG_ERROR);

#define VFMT(v) (v)>>16, (v)>>8 & 0xff, (v) & 0xff
    for (i = 0; i < FF_ARRAY_ELEMS(fflibs); i++) {
        const unsigned bversion = fflibs[i].build_version;
        const unsigned rversion = fflibs[i].runtime_version;
        INFO(s, "lib%-12s build:%3d.%3d.%3d runtime:%3d.%3d.%3d",
             fflibs[i].libname, VFMT(bversion), VFMT(rversion));
        if (bversion != rversion)
            fprintf(stderr, "WARNING: build and runtime version of FFmpeg mismatch\n");
    }

    av_register_all();
    avfilter_register_all();

    av_opt_set_defaults(s);

    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);

    /* At least first_ts and last_pushed_frame_ts must be kept between full
     * runs so we can not set this initialization in configure_context() */
    s->first_ts = AV_NOPTS_VALUE;
    s->last_pushed_frame_ts = AV_NOPTS_VALUE;
    s->trim_duration64 = AV_NOPTS_VALUE;

    av_assert0(!s->context_configured);
    return s;

fail:
    free_context(s);
    return NULL;
}

/* If decoding thread is dying (EOF reached for example), wait for it to end
 * and confirm dead state */
static int join_dec_thread_if_dying(struct sxplayer_ctx *s)
{
    int ret;

    pthread_mutex_lock(&s->lock);
    if (s->thread_state == THREAD_STATE_DYING) {
        TRACE(s, "thread is dying: join");
        pthread_mutex_unlock(&s->lock);

        async_wait(s->actx);

        pthread_mutex_lock(&s->lock);
        s->thread_state = THREAD_STATE_NOTRUNNING;
        free_temp_context_data(s);
        ret = 2;
    } else {
        ret = s->thread_state == THREAD_STATE_NOTRUNNING;
    }
    pthread_mutex_unlock(&s->lock);
    return ret;
}

static void shoot_running_decoding_thread(struct sxplayer_ctx *s)
{
    pthread_mutex_lock(&s->lock);
    if (s->thread_state == THREAD_STATE_RUNNING) {
        TRACE(s, "decoding thread is running, *BANG*");
        s->thread_state = THREAD_STATE_DYING;
        pthread_cond_signal(&s->cond);
    }
    pthread_mutex_unlock(&s->lock);
}

void sxplayer_free(struct sxplayer_ctx **ss)
{
    struct sxplayer_ctx *s = *ss;

    TRACE(s, "destroying context");

    if (!s)
        return;

    shoot_running_decoding_thread(s);
    join_dec_thread_if_dying(s);

    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);

    free_temp_context_data(s);
    free_context(s);
    *ss = NULL;
}

/**
 * Setup the libavfilter filtergraph for user filter but also to have a way to
 * request a pixel format we want, and let libavfilter insert the necessary
 * scaling filter (typically, an automatic conversion from yuv420p to rgb32).
 */
static int setup_filtergraph(struct sxplayer_ctx *s)
{
    int ret = 0;
    char args[512];
    AVRational framerate;
    AVFilter *buffersrc, *buffersink;
    AVFilterInOut *outputs, *inputs;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->last_frame_format);
    const AVCodecContext *avctx = s->dec_ctx->avctx;

    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)
        return 0;

    avfilter_graph_free(&s->filter_graph);

    outputs = avfilter_inout_alloc();
    inputs  = avfilter_inout_alloc();

    buffersrc  = avfilter_get_by_name(s->media_type == AVMEDIA_TYPE_VIDEO ? "buffer" : "abuffer");
    buffersink = avfilter_get_by_name(s->media_type == AVMEDIA_TYPE_VIDEO ? "buffersink" : "abuffersink");

    s->filter_graph = avfilter_graph_alloc();

    if (!inputs || !outputs || !s->filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    inputs->name  = av_strdup("out");
    outputs->name = av_strdup("in");
    if (!inputs->name || !outputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* create buffer filter source (where we push the frame) */
    if (s->media_type == AVMEDIA_TYPE_VIDEO) {
        const AVRational time_base = AV_TIME_BASE_Q;
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%s:time_base=%d/%d:pixel_aspect=%d/%d:sws_param=flags=bicubic",
                 avctx->width, avctx->height, av_get_pix_fmt_name(s->last_frame_format),
                 time_base.num, time_base.den,
                 avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);
    } else {
        snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s",
                 1, avctx->sample_rate, avctx->sample_rate,
                 av_get_sample_fmt_name(avctx->sample_fmt));
        if (avctx->channel_layout)
            av_strlcatf(args, sizeof(args), ":channel_layout=0x%"PRIx64, avctx->channel_layout);
        else
            av_strlcatf(args, sizeof(args), ":channels=%d", avctx->channels);
    }

    framerate = av_guess_frame_rate(s->fmt_ctx, s->stream, NULL);
    if (framerate.num && framerate.den)
        av_strlcatf(args, sizeof(args), ":frame_rate=%d/%d", framerate.num, framerate.den);

    TRACE(s, "graph buffer source args: %s", args);

    ret = avfilter_graph_create_filter(&s->buffersrc_ctx, buffersrc,
                                       outputs->name, args, NULL, s->filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Unable to create buffer filter source\n");
        goto end;
    }

    /* create buffer filter sink (where we pull the frame) */
    ret = avfilter_graph_create_filter(&s->buffersink_ctx, buffersink,
                                       inputs->name, NULL, NULL, s->filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Unable to create buffer filter sink\n");
        goto end;
    }

    /* define the output of the graph */
    snprintf(args, sizeof(args), "%s", s->filters ? s->filters : "");
    if (s->media_type == AVMEDIA_TYPE_VIDEO) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->last_frame_format);
        const enum AVPixelFormat sw_pix_fmt = pix_fmts_sx2ff(s->sw_pix_fmt);
        const enum AVPixelFormat pix_fmt = !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL) ? sw_pix_fmt : s->last_frame_format;
        av_strlcatf(args, sizeof(args), "%sformat=%s", *args ? "," : "", av_get_pix_fmt_name(pix_fmt));
    } else {
        av_strlcatf(args, sizeof(args), "aformat=sample_fmts=fltp:channel_layouts=stereo, asetnsamples=%d", AUDIO_NBSAMPLES);
    }

    TRACE(s, "graph buffer sink args: %s", args);

    /* create our filter graph */
    inputs->filter_ctx  = s->buffersink_ctx;
    outputs->filter_ctx = s->buffersrc_ctx;

    ret = avfilter_graph_parse_ptr(s->filter_graph, args, &inputs, &outputs, NULL);
    if (ret < 0)
        goto end;

    ret = avfilter_graph_config(s->filter_graph, NULL);
    if (ret < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

static double get_rotation(AVStream *st)
{
    AVDictionaryEntry *rotate_tag = av_dict_get(st->metadata, "rotate", NULL, 0);
    uint8_t* displaymatrix = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, NULL);
    double theta = 0;

    if (rotate_tag && *rotate_tag->value && strcmp(rotate_tag->value, "0")) {
        char *tail;
        theta = av_strtod(rotate_tag->value, &tail);
        if (*tail)
            theta = 0;
    }
    if (displaymatrix && !theta)
        theta = -av_display_rotation_get((int32_t *)displaymatrix);
    theta -= 360*floor(theta/360 + 0.9/360);
    return theta;
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

static int pull_packet_cb(void *priv, AVPacket *pkt)
{
    int ret;
    struct sxplayer_ctx *s = priv;
    AVFormatContext *fmt_ctx = s->fmt_ctx;
    const int target_stream_idx = s->stream->index;

    for (;;) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0)
            break;

        if (pkt->stream_index != target_stream_idx) {
            TRACE(s, "pkt->idx=%d vs %d",
                  pkt->stream_index, target_stream_idx);
            av_packet_unref(pkt);
            continue;
        }

        if (s->pkt_skip_mod) {
            s->pkt_count++;
            if (s->pkt_count % s->pkt_skip_mod && !(pkt->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(pkt);
                continue;
            }
        }

        break;
    }

    TRACE(s, "packet ret %s", av_err2str(ret));
    return ret;
}

/**
 * Map the timeline time to the media time
 */
static int64_t get_media_time(const struct sxplayer_ctx *s, int64_t t)
{
    if (s->trim_duration64 < 0)
        return 0;
    return s->skip64 + FFMIN(t, s->trim_duration64);
}

/**
 * Request a seek to the demuxer
 */
static int seek_cb(void *arg, int64_t t)
{
    struct sxplayer_ctx *s = arg;

    INFO(s, "Seek in media at ts=%s", PTS2TIMESTR(t));
    return avformat_seek_file(s->fmt_ctx, -1, INT64_MIN, t, t, 0);
}

/**
 * Convert an audio frame (PCM data) to a textured video frame with waves and
 * FFT lines
 */
static void audio_frame_to_sound_texture(struct sxplayer_ctx *s, AVFrame *dst_video,
                                         const AVFrame *audio_src)
{
    int i, j, ch;
    const int nb_samples = audio_src->nb_samples;
    const int width = nb_samples / 2;
    const float scale = 1.f / sqrt(AUDIO_NBSAMPLES/2 + 1);

    memset(dst_video->data[0], 0, dst_video->height * dst_video->linesize[0]);

    /* Copy waves */
    for (ch = 0; ch < AUDIO_NBCHANNELS; ch++) {
        const int lz = dst_video->linesize[0];
        float *samples_dst = (float *)(dst_video->data[0] + ch * lz);
        const float *samples_src = (const float *)audio_src->extended_data[ch];

        for (i = 0; i < width; i++)
            samples_dst[i] = (samples_src[width/2 + i] + 1.f) / 2.f;
    }

    /* Fourier transform */
    for (ch = 0; ch < AUDIO_NBCHANNELS; ch++) {
        const int lz = dst_video->linesize[0];
        float *fft_dst = (float *)(dst_video->data[0] + (AUDIO_NBCHANNELS + ch) * lz);
        const float *samples_src = (const float *)audio_src->extended_data[ch];
        float *bins = s->rdft_data[ch];

        /* Apply window function to input samples */
        for (i = 0; i < nb_samples; i++)
            bins[i] = samples_src[i] * s->window_func_lut[i];

        /* Run transform.
         *
         * After av_rdft_calc(), the bins is an array of successive real and
         * imaginary floats, except for the first two bins which are
         * respectively the real corresponding to the lower frequency and the
         * real for the higher frequency.
         *
         * The imaginary parts for these two frequencies are always 0 so they
         * are assumed as such. This trick allowed an in-place processing for
         * the N samples into N+1 complex.
         */
        av_rdft_calc(s->rdft, bins);

        /* Get magnitude of frequency bins and copy result into texture
         *
         * Note: since we only have space for N samples in the texture, we skip
         * the first complex (lower frequency one).
         */
#define MAGNITUDE(re, im) sqrtf(((re)*(re) + (im)*(im)) * scale)
        for (i = 1; i < width - 1; i++)
            fft_dst[i] = MAGNITUDE(bins[2*i], bins[2*i + 1]);

        /* Last complex (higher frequency one) is one of the the special case
         * mentioned above */
        fft_dst[width - 1] = MAGNITUDE(bins[1], 0);
    }

    /* Downscaled versions of the FFT */
    for (i = 0; i < AUDIO_NBITS-1; i++) {
        for (ch = 0; ch < AUDIO_NBCHANNELS; ch++) {
            const int lz = dst_video->linesize[0];
            const int source_line = (i + 1)*AUDIO_NBCHANNELS + ch;
            float *fft_src = (float *)(dst_video->data[0] +  source_line                     * lz);
            float *fft_dst = (float *)(dst_video->data[0] + (source_line + AUDIO_NBCHANNELS) * lz);

            const int source_step = 1 << i;
            const int nb_identical_values = source_step << 1;
            const int nb_dest_pixels = width / nb_identical_values;

            TRACE(s, "line %2d->%2d: %3d different pixels (copied %3dx) as destination, step source: %d",
                  source_line, source_line + AUDIO_NBCHANNELS, nb_dest_pixels, nb_identical_values, source_step);

            for (j = 0; j < nb_dest_pixels; j++) {
                int x;
                const float avg = (fft_src[ j*2      * source_step] +
                                   fft_src[(j*2 + 1) * source_step]) / 2.f;

                for (x = 0; x < nb_identical_values; x++)
                    fft_dst[j*nb_identical_values + x] = avg;
            }
        }
    }
}

static int filter_frame(struct sxplayer_ctx *s, AVFrame *outframe, AVFrame *inframe)
{
    int ret = 0; //, done = 0;

    /* lazy filtergraph configuration: we need to wait for the first
     * frame to see what pixel format is getting decoded (no other way
     * with hardware acceleration apparently) */
    if (inframe) {
        // XXX: check width/height changes?
        if (s->last_frame_format != inframe->format) {
            s->last_frame_format = inframe->format;
            ret = setup_filtergraph(s);
            if (ret < 0)
                return ret;
        }
    }

    if (inframe) {
        TRACE(s, "received %s frame @ ts=%s",
            s->media_type == AVMEDIA_TYPE_VIDEO ? av_get_pix_fmt_name(inframe->format)
                                                : av_get_sample_fmt_name(inframe->format),
            PTS2TIMESTR(inframe->pts));

        if (s->filter_graph) {
            TRACE(s, "push frame with ts=%s into filtergraph", PTS2TIMESTR(inframe->pts));

            /* push the decoded frame into the filtergraph */
            ret = av_buffersrc_write_frame(s->buffersrc_ctx, inframe);
            if (ret < 0) {
                fprintf(stderr, "Error while feeding the filtergraph\n");
                return ret;
            }

            /* the frame is sent into the filtergraph, we don't need it anymore */
            av_frame_unref(inframe);
        }
    } else {
        if (s->filter_graph) {
            ret = av_buffersrc_write_frame(s->buffersrc_ctx, NULL);
            if (ret < 0) {
                fprintf(stderr, "Error while feeding null frame in the filtergraph\n");
                return ret;
            }
        }
    }

    /* try to get a frame from the filtergraph */
    AVFrame *filtered_frame = s->media_type == AVMEDIA_TYPE_AUDIO ? s->tmp_audio_frame : outframe;
    if (s->filter_graph) {
        ret = av_buffersink_get_frame(s->buffersink_ctx, filtered_frame);
        TRACE(s, "got frame from sink ret=[%s]", av_err2str(ret));
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                fprintf(stderr, "Error while pulling the frame from the filtergraph\n");
        }
    } else {
        if (!inframe) {
            ret = AVERROR_EOF;
        } else {
            av_frame_move_ref(filtered_frame, inframe);
        }
    }

    if (s->media_type == AVMEDIA_TYPE_AUDIO) {
        audio_frame_to_sound_texture(s, s->audio_texture_frame, filtered_frame);
        av_frame_unref(filtered_frame);
        av_frame_ref(outframe, s->audio_texture_frame);
    }

    TRACE(s, "outframe: %s frame @ ts=%s %"PRId64"",
          av_get_pix_fmt_name(outframe->format), PTS2TIMESTR(outframe->pts));

    return ret;
}

static int push_frame_cb(void *priv, AVFrame *frame)
{
    int ret;
    struct sxplayer_ctx *s = priv;
    const int flush = !frame;

    if (frame) {
        TRACE(s, "got frame (in %s) with t=%s",
            s->media_type == AVMEDIA_TYPE_VIDEO ? av_get_pix_fmt_name(frame->format)
                                                : av_get_sample_fmt_name(frame->format),
            PTS2TIMESTR(frame->pts));
    } else {
        TRACE(s, "got null frame");
    }

    ret = filter_frame(s, s->filtered_frame, frame);
    av_frame_free(&frame);
    TRACE(s, "filter_frame returned [%s]", av_err2str(ret));
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            /* frame was pushed in filtegraph but nothing ready yet */
            return 0;
        }
        if (ret == AVERROR_EOF) {
            /* filtergraph returned EOF so we can call for a terminate */
            TRACE(s, "GOT EOF from filtergraph");
            shoot_running_decoding_thread(s);
        }
        return ret;
    }

    TRACE(s, "frame filtered");

    if (s->trim_duration64 >= 0) {
        if (s->filtered_frame->pts > s->skip64 + s->trim_duration64) {
            TRACE(s, "reached trim duration, simulate EOF");
            av_frame_unref(s->filtered_frame);
            shoot_running_decoding_thread(s);
            return AVERROR_EOF;
        }
    }

    /* wait for the frame to be consumed by the main thread */
    pthread_mutex_lock(&s->lock);
    while (s->queued_frame && s->thread_state != THREAD_STATE_DYING) {
        TRACE(s, "frame not poped out yet, wait");
        pthread_cond_wait(&s->cond, &s->lock);
    }
    if (s->thread_state == THREAD_STATE_DYING) {
        TRACE(s, "thread is dying");
        av_frame_free(&s->queued_frame);
        // XXX: no need to signal cond, right?
        pthread_mutex_unlock(&s->lock);
        av_frame_unref(s->filtered_frame);
        return AVERROR_EOF;
    }

    TRACE(s, "queuing frame");
    s->queued_frame = av_frame_clone(s->filtered_frame); // XXX
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
    av_frame_unref(s->filtered_frame);

    if (flush) {
        TRACE(s, "EOF/null frame was signaled but we got a frame out "
              "of the filtergraph, so request a flush call again");
        return AVERROR(EAGAIN);
    }

    return 0;
}

/**
 * Open the input file.
 */
static int open_ifile(struct sxplayer_ctx *s, const char *infile)
{
    int ret;
    const struct decoder *dec_def, *dec_def_fallback;

    if (s->auto_hwaccel && decoder_def_hwaccel) {
        dec_def          = decoder_def_hwaccel;
        dec_def_fallback = decoder_def_software;
    } else {
        dec_def          = decoder_def_software;
        dec_def_fallback = NULL;
    }

    av_assert0(!s->actx);
    s->actx = async_alloc_context();
    if (!s->actx)
        return AVERROR(ENOMEM);

    TRACE(s, "opening %s", infile);
    ret = avformat_open_input(&s->fmt_ctx, infile, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Unable to open input file '%s'\n", infile);
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
    s->fmt_ctx->max_analyze_duration = -1;

    TRACE(s, "find stream info");
    ret = avformat_find_stream_info(s->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Unable to find input stream information\n");
        return ret;
    }

    TRACE(s, "find best stream");
    ret = av_find_best_stream(s->fmt_ctx, s->media_type, -1, -1, &s->dec, 0);
    if (ret < 0) {
        fprintf(stderr, "Unable to find a %s stream in the input file\n", s->media_type_string);
        return ret;
    }
    s->stream_idx = ret;
    s->stream = s->fmt_ctx->streams[s->stream_idx];

    TRACE(s, "create decoder");
    s->dec_ctx = decoder_create(dec_def, dec_def_fallback, s->stream->codec);
    if (!s->dec_ctx)
        return AVERROR(ENOMEM);

    TRACE(s, "register reader");
    ret = async_register_reader(s->actx, s,
                                pull_packet_cb, seek_cb,
                                &s->reader);
    if (ret < 0)
        return ret;

    s->last_frame_format = AV_PIX_FMT_NONE;

    // XXX: check tb.den!=0?
    ret = async_register_decoder(s->reader, s->dec_ctx, s,
                                 push_frame_cb, &s->adec,
                                 s->stream->time_base);
    if (ret < 0)
        return ret;

    // XXX
    s->dec_ctx->adec = s->adec;

    av_opt_set_int(s->adec, "max_frames_queue",  s->max_nb_frames,  0);
    av_opt_set_int(s->adec, "max_packets_queue", s->max_nb_packets, 0);

    s->trim_duration64 = s->trim_duration < 0 ? AV_NOPTS_VALUE : TIME2INT64(s->trim_duration);

    if (!strstr(s->fmt_ctx->iformat->name, "image2") && !strstr(s->fmt_ctx->iformat->name, "_pipe")) {
        int64_t probe_duration64 = s->fmt_ctx->duration;
        AVRational scaleq = AV_TIME_BASE_Q;
        if (probe_duration64 == AV_NOPTS_VALUE && s->stream->time_base.den) {
            probe_duration64 = s->stream->duration;
            scaleq = s->stream->time_base;
        }

        if (probe_duration64 != AV_NOPTS_VALUE) {
            const double probe_duration = probe_duration64 * av_q2d(scaleq);

            if (s->trim_duration < 0 || probe_duration < s->trim_duration) {
                TRACE(s, "fix trim_duration from %f to %f", s->trim_duration, probe_duration);
                s->trim_duration64 = av_rescale_q_rnd(probe_duration64, scaleq, AV_TIME_BASE_Q, 0);
                s->trim_duration = probe_duration;
            }
        }
    }

    TRACE(s, "rescaled values: skip=%s dist:%s dur:%s",
          PTS2TIMESTR(s->skip64),
          PTS2TIMESTR(s->dist_time_seek_trigger64),
          PTS2TIMESTR(s->trim_duration64));

    if (s->autorotate) {
        const double theta = get_rotation(s->stream);

        if (fabs(theta - 90) < 1.0)
            s->filters = update_filters_str(s->filters, "transpose=clock");
        else if (fabs(theta - 180) < 1.0)
            s->filters = update_filters_str(s->filters, "vflip,hflip");
        else if (fabs(theta - 270) < 1.0)
            s->filters = update_filters_str(s->filters, "transpose=cclock");
        TRACE(s, "update filtergraph to: %s", s->filters);
    }

    av_dump_format(s->fmt_ctx, 0, infile, 0);

    s->context_configured = 1;

    return 0;
}

static int set_context_fields(struct sxplayer_ctx *s)
{
    if (pix_fmts_sx2ff(s->sw_pix_fmt) == AV_PIX_FMT_NONE) {
        fprintf(stderr, "Invalid software decoding pixel format specified\n");
        return AVERROR(EINVAL);
    }

    switch (s->avselect) {
    case SXPLAYER_SELECT_VIDEO: s->media_type = AVMEDIA_TYPE_VIDEO; break;
    case SXPLAYER_SELECT_AUDIO: s->media_type = AVMEDIA_TYPE_AUDIO; break;
    default:
        av_assert0(0);
    }

    s->media_type_string = av_get_media_type_string(s->media_type);
    av_assert0(s->media_type_string);

    s->thread_state = THREAD_STATE_NOTRUNNING;

    if (s->auto_hwaccel && (s->filters || s->autorotate || s->export_mvs)) {
        fprintf(stderr, "Filters ('%s'), autorotate (%d), or export_mvs (%d) settings "
                "are set but hwaccel is enabled, disabling auto_hwaccel so these "
                "options are honored\n", s->filters, s->autorotate, s->export_mvs);
        s->auto_hwaccel = 0;
    }

    TRACE(s, "avselect:%s skip:%f trim_duration:%f "
          "dist_time_seek_trigger:%f max_nb_frames:%d filters:'%s'",
          s->media_type_string, s->skip, s->trim_duration,
          s->dist_time_seek_trigger, s->max_nb_frames, s->filters ? s->filters : "");

    s->skip64 = TIME2INT64(s->skip);
    s->dist_time_seek_trigger64 = TIME2INT64(s->dist_time_seek_trigger);

    s->filtered_frame = av_frame_alloc();
    if (!s->filtered_frame)
        return AVERROR(ENOMEM);

    if (s->media_type == AVMEDIA_TYPE_AUDIO) {
        s->audio_texture_frame = get_audio_frame();
        if (!s->audio_texture_frame)
            return AVERROR(ENOMEM);
        s->tmp_audio_frame = av_frame_alloc();
        if (!s->tmp_audio_frame)
            return AVERROR(ENOMEM);
    }

    if (s->media_type == AVMEDIA_TYPE_AUDIO) {
        int i;

        /* Pre-calc windowing function */
        s->window_func_lut = av_malloc_array(AUDIO_NBSAMPLES, sizeof(*s->window_func_lut));
        if (!s->window_func_lut)
            return AVERROR(ENOMEM);
        for (i = 0; i < AUDIO_NBSAMPLES; i++)
            s->window_func_lut[i] = .5f * (1 - cos(2*M_PI*i / (AUDIO_NBSAMPLES-1)));

        /* Real Discrete Fourier Transform context (Real to Complex) */
        s->rdft = av_rdft_init(AUDIO_NBITS, DFT_R2C);
        if (!s->rdft) {
            fprintf(stderr, "Unable to init RDFT context with N=%d\n", AUDIO_NBITS);
            return AVERROR(ENOMEM);
        }

        s->rdft_data[0] = av_mallocz_array(AUDIO_NBSAMPLES, sizeof(*s->rdft_data[0]));
        s->rdft_data[1] = av_mallocz_array(AUDIO_NBSAMPLES, sizeof(*s->rdft_data[1]));
        if (!s->rdft_data[0] || !s->rdft_data[1])
            return AVERROR(ENOMEM);
    }

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

    if (s->context_configured)
        return 0;

    TRACE(s, "set context fields");
    ret = set_context_fields(s);
    if (ret < 0) {
        fprintf(stderr, "Unable to set context fields: %s\n", av_err2str(ret));
        free_temp_context_data(s);
        return ret;
    }

    TRACE(s, "open input file");
    ret = open_ifile(s, s->filename);
    if (ret < 0) {
        fprintf(stderr, "Unable to open input file: %s\n", av_err2str(ret));
        free_temp_context_data(s);
        return ret;
    }

    return 0;
}

/* Return the frame only if different from previous one. We do not make a
 * simple pointer check because of the frame reference counting (and thus
 * pointer reuse, depending on many parameters)  */
static struct sxplayer_frame *ret_frame(struct sxplayer_ctx *s, AVFrame *frame, int64_t req_t)
{
    struct sxplayer_frame *ret;
    AVFrameSideData *sd;

    if (!frame) {
        INFO(s, " <<< return nothing");
        return NULL;
    }

    const int64_t frame_ts = frame->pts;

    TRACE(s, "last_pushed_frame_ts:%s frame_ts:%s",
          PTS2TIMESTR(s->last_pushed_frame_ts), PTS2TIMESTR(frame_ts));

    /* if same frame as previously, do not raise it again */
    if (s->last_pushed_frame_ts == frame_ts) {
        INFO(s, " <<< same frame as previously, return NULL");
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
            fprintf(stderr, "Unable to memdup motion vectors side data\n");
            av_free(ret);
            return NULL;
        }
        ret->nb_mvs = sd->size / sizeof(AVMotionVector);
        TRACE(s, "export %d motion vectors", ret->nb_mvs);
    }

    ret->internal = frame;
    ret->data     = frame->data[frame->format == AV_PIX_FMT_VIDEOTOOLBOX ? 3 : 0];
    ret->linesize = frame->linesize[0];
    ret->width    = frame->width;
    ret->height   = frame->height;
    ret->ts       = frame_ts * av_q2d(AV_TIME_BASE_Q);
    ret->pix_fmt  = pix_fmts_ff2sx(frame->format);

    INFO(s, " <<< return frame @ ts=%s with requested time being %s [max:%s]",
         PTS2TIMESTR(frame_ts), PTS2TIMESTR(req_t), PTS2TIMESTR(s->skip64 + s->trim_duration64));
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
    if (!s)
        return -1;

#if 0
    pthread_mutex_lock(&s->queue_lock);
    TRACE(s, "toggle drop frame from %d to %d", s->request_drop, drop);
    s->request_drop = drop;
    pthread_mutex_unlock(&s->queue_lock);
    return 0;
#else
    return -1;
#endif
}

static int spawn_decoding_thread_if_not_running(struct sxplayer_ctx *s)
{
    int ret = 0;

    if (join_dec_thread_if_dying(s)) {
        TRACE(s, "dec thread is dead, start it");

        ret = configure_context(s);
        if (ret < 0)
            return ret;

        s->thread_state = THREAD_STATE_RUNNING;

        if (s->skip64)
            async_reader_seek(s->reader, s->skip64);
        async_start(s->actx);

        s->request_drop = -1;
    }

    return ret;
}

/* Must be called by main thread only */
static AVFrame *pop_frame(struct sxplayer_ctx *s)
{
    AVFrame *frame = NULL;

    if (s->cached_frame) {
        TRACE(s, "we have a cached frame, pop this one");
        frame = s->cached_frame;
        s->cached_frame = NULL;
    } else {
        TRACE(s, "requesting a frame");
        pthread_mutex_lock(&s->lock);
        while (!s->queued_frame && s->thread_state == THREAD_STATE_RUNNING) {
            TRACE(s, "no frame yet, wait");
            pthread_cond_wait(&s->cond, &s->lock);
        }
        if (s->queued_frame) {
            frame = s->queued_frame;
            s->queued_frame = NULL;
        }
        pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->lock);
    }

    if (frame) {
        const int64_t ts = frame->pts;
        TRACE(s, "poped queued frame with ts=%s", PTS2TIMESTR(ts));
        if (s->first_ts == AV_NOPTS_VALUE)
            s->first_ts = ts;
    } else {
        TRACE(s, "no frame available");
    }

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
    return ret_frame(s, frame, t64);
}
#endif

struct sxplayer_frame *sxplayer_get_frame(struct sxplayer_ctx *s, double t)
{
    int ret;
    int64_t diff;
    const int64_t t64 = TIME2INT64(t);

    INFO(s, " >>> get frame for t=%s (user exact value:%f)", PTS2TIMESTR(t64), t);

#if SYNTH_FRAME
    return ret_synth_frame(s, t);
#endif

    ret = configure_context(s);
    if (ret < 0)
        return NULL;

    const int vt = get_media_time(s, t64);
    TRACE(s, "t=%s -> vt=%s", PTS2TIMESTR(t64), PTS2TIMESTR(vt));

    if (t64 < 0) {
        TRACE(s, "prefetch requested");
        spawn_decoding_thread_if_not_running(s);
        return ret_frame(s, NULL, vt);
    }

    /* If the trim duration couldn't be evaluated, it's likely an image so
     * we will assume this is the case. In the case we already pushed a
     * picture we don't want to restart the decoding thread again so we
     * return NULL. */
    if (s->trim_duration64 < 0 && s->last_pushed_frame_ts != AV_NOPTS_VALUE) {
        TRACE(s, "no trim duration, likely picture, and frame already returned");
        return ret_frame(s, NULL, vt);
    }

    AVFrame *candidate = NULL;

    /* If no frame was ever pushed, we need to pop one */
    if (s->last_pushed_frame_ts == AV_NOPTS_VALUE) {
        TRACE(s, "no frame ever pushed yet, pop a candidate");
        spawn_decoding_thread_if_not_running(s);
        candidate = pop_frame(s);
        if (!candidate) {
            TRACE(s, "can not get a single frame for this media");
            return ret_frame(s, NULL, vt);
        }

        diff = vt - candidate->pts;
        TRACE(s, "diff with candidate (t=%s): %s [%"PRId64"]",
              PTS2TIMESTR(candidate->pts), PTS2TIMESTR(diff), diff);
    } else {
        diff = vt - s->last_pushed_frame_ts;
        TRACE(s, "diff with latest frame (t=%s) returned: %s [%"PRId64"]",
              PTS2TIMESTR(s->last_pushed_frame_ts), PTS2TIMESTR(diff), diff);
    }

    //XXX: why not?? erg audio, first frame nopts
    //av_assert0(s->first_ts != AV_NOPTS_VALUE);

    if (!diff)
        return ret_frame(s, candidate, vt);

    /* The first timestamp as been obtained (either by poping a frame earlier
     * in this function, or because a frame was already pushed), so we can
     * check if the timestamp requested is before the first visible frame */
    if (vt < s->first_ts) {
        TRACE(s, "requested a time before the first video timestamp");
        // The frame poped needs to be cached before the requested timestamps
        // reaches this candidate

        //av_assert0(!s->cached_frame);
        av_frame_free(&s->cached_frame); // XXX: why? :(

        s->cached_frame = candidate;
        return ret_frame(s, NULL, vt);
    }

    /* Check if a seek is needed */
    if (diff < 0 || diff > s->dist_time_seek_trigger64) {

        if (diff < 0)
            TRACE(s, "diff %s [%"PRId64"] < 0 request backward seek", PTS2TIMESTR(diff), diff);
        else
            TRACE(s, "diff %s > %s [%"PRId64" > %"PRId64"] request future seek",
                  PTS2TIMESTR(diff), PTS2TIMESTR(s->dist_time_seek_trigger64),
                  diff, s->dist_time_seek_trigger64);

        spawn_decoding_thread_if_not_running(s);

        async_reader_seek(s->reader, get_media_time(s, t64));
        av_frame_free(&candidate);

        /* If the seek is backward, wait for it to be effective */
        TRACE(s, "backward seek requested, wait for it to be effective");
        if (diff < 0) {
            do {
                AVFrame *next = pop_frame(s);
                if (!next)
                    break;
                av_frame_free(&candidate);
                candidate = next;
            } while (candidate->pts > vt);
        }
    }

    if (!join_dec_thread_if_dying(s)) {

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
    }

    return ret_frame(s, candidate, vt);
}

struct sxplayer_frame *sxplayer_get_next_frame(struct sxplayer_ctx *s)
{
    int ret;

    TRACE(s, " >>> get next frame");

    ret = configure_context(s);
    if (ret < 0)
        return NULL;

    /* If we are joining (and killing) the thread, it means it reached a EOF,
     * and this is the only reason to return NULL in this function. */
    if (join_dec_thread_if_dying(s) == 2) {
        TRACE(s, "thread died, return EOF");
        return ret_frame(s, NULL, 0);
    }

    spawn_decoding_thread_if_not_running(s);
    return ret_frame(s, pop_frame(s), 0);
}

int sxplayer_get_duration(struct sxplayer_ctx *s, double *duration)
{
    int ret;

    INFO(s, "query duration");
    ret = configure_context(s);
    if (ret < 0)
        return ret;
    *duration = s->trim_duration;
    TRACE(s, "get duration -> %f", s->trim_duration);
    return 0;
}
