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

#include <libavcodec/avfft.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avstring.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/timestamp.h>

#include "sxplayer.h"
#include "internal.h"
#include "filtering.h"

#define AUDIO_NBITS      10
#define AUDIO_NBSAMPLES  (1<<(AUDIO_NBITS))
#define AUDIO_NBCHANNELS 2

struct filtering_ctx {
    const AVClass *class;

    AVThreadMessageQueue *in_queue;
    AVThreadMessageQueue *out_queue;
    AVCodecContext *avctx;              // TODO remove
    int sw_pix_fmt;                     // TODO remove

    /* options */
    char *filters;
    int64_t max_pts;

    AVFilterGraph *filter_graph;
    AVFrame *filtered_frame;
    AVFrame *audio_texture_frame;
    AVFrame *tmp_audio_frame;
    enum AVPixelFormat last_frame_format;
    AVFilterContext *buffersink_ctx;        // sink of the graph (from where we pull)
    AVFilterContext *buffersrc_ctx;         // source of the graph (where we push)
    float *window_func_lut;                 // audio window function lookup table
    RDFTContext *rdft;                      // real discrete fourier transform context
    FFTSample *rdft_data[AUDIO_NBCHANNELS]; // real discrete fourier transform data for each channel
};

#define OFFSET_DEC(x) offsetof(struct filtering_ctx, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM
static const AVOption filtering_options[] = {
    { "filters", NULL,  OFFSET_DEC(filters), AV_OPT_TYPE_STRING, {.str=NULL},           CHAR_MIN,  CHAR_MAX,  FLAGS },
    { "max_pts", NULL,  OFFSET_DEC(max_pts), AV_OPT_TYPE_INT64,  {.i64=AV_NOPTS_VALUE}, INT64_MIN, INT64_MAX, FLAGS },
    { NULL }
};

static const AVClass filtering_class = {
    .class_name = "filtering",
    .item_name  = av_default_item_name,
    .option     = filtering_options,
};

struct filtering_ctx *filtering_alloc(void)
{
    struct filtering_ctx *f = av_mallocz(sizeof(*f));
    if (!f)
        return NULL;
    f->class = &filtering_class;
    return f;
}

void filtering_free(struct filtering_ctx **fp)
{
    av_freep(fp);
}

/**
 * Convert an audio frame (PCM data) to a textured video frame with waves and
 * FFT lines
 */
static void audio_frame_to_sound_texture(struct filtering_ctx *f, AVFrame *dst_video,
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
        float *bins = f->rdft_data[ch];

        /* Apply window function to input samples */
        for (i = 0; i < nb_samples; i++)
            bins[i] = samples_src[i] * f->window_func_lut[i];

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
        av_rdft_calc(f->rdft, bins);

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

            TRACE(f, "line %2d->%2d: %3d different pixels (copied %3dx) as destination, step source: %d",
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

/**
 * Setup the libavfilter filtergraph for user filter but also to have a way to
 * request a pixel format we want, and let libavfilter insert the necessary
 * scaling filter (typically, an automatic conversion from yuv420p to rgb32).
 */
static int setup_filtergraph(struct filtering_ctx *f)
{
    int ret = 0;
    char args[512];
    //AVRational framerate;
    AVFilter *buffersrc, *buffersink;
    AVFilterInOut *outputs, *inputs;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(f->last_frame_format);
    const AVCodecContext *avctx = f->avctx;

    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)
        return 0;

    avfilter_graph_free(&f->filter_graph);

    outputs = avfilter_inout_alloc();
    inputs  = avfilter_inout_alloc();

    buffersrc  = avfilter_get_by_name(avctx->codec_type == AVMEDIA_TYPE_VIDEO ? "buffer" : "abuffer");
    buffersink = avfilter_get_by_name(avctx->codec_type == AVMEDIA_TYPE_VIDEO ? "buffersink" : "abuffersink");

    f->filter_graph = avfilter_graph_alloc();

    if (!inputs || !outputs || !f->filter_graph) {
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
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        const AVRational time_base = AV_TIME_BASE_Q;
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%s:time_base=%d/%d:pixel_aspect=%d/%d:sws_param=flags=bicubic",
                 avctx->width, avctx->height, av_get_pix_fmt_name(f->last_frame_format),
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

#if 0
    framerate = av_guess_frame_rate(s->fmt_ctx, s->stream, NULL);
    if (framerate.num && framerate.den)
        av_strlcatf(args, sizeof(args), ":frame_rate=%d/%d", framerate.num, framerate.den);
#endif

    TRACE(f, "graph buffer source args: %s", args);

    ret = avfilter_graph_create_filter(&f->buffersrc_ctx, buffersrc,
                                       outputs->name, args, NULL, f->filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Unable to create buffer filter source\n");
        goto end;
    }

    /* create buffer filter sink (where we pull the frame) */
    ret = avfilter_graph_create_filter(&f->buffersink_ctx, buffersink,
                                       inputs->name, NULL, NULL, f->filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Unable to create buffer filter sink\n");
        goto end;
    }

    /* define the output of the graph */
    snprintf(args, sizeof(args), "%s", f->filters ? f->filters : "");
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(f->last_frame_format);
        const enum AVPixelFormat sw_pix_fmt = pix_fmts_sx2ff(f->sw_pix_fmt);
        const enum AVPixelFormat pix_fmt = !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL) ? sw_pix_fmt : f->last_frame_format;
        av_strlcatf(args, sizeof(args), "%sformat=%s", *args ? "," : "", av_get_pix_fmt_name(pix_fmt));
    } else {
        av_strlcatf(args, sizeof(args), "aformat=sample_fmts=fltp:channel_layouts=stereo, asetnsamples=%d", AUDIO_NBSAMPLES);
    }

    TRACE(f, "graph buffer sink args: %s", args);

    /* create our filter graph */
    inputs->filter_ctx  = f->buffersink_ctx;
    outputs->filter_ctx = f->buffersrc_ctx;

    ret = avfilter_graph_parse_ptr(f->filter_graph, args, &inputs, &outputs, NULL);
    if (ret < 0)
        goto end;

    ret = avfilter_graph_config(f->filter_graph, NULL);
    if (ret < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

static int filter_frame(struct filtering_ctx *f, AVFrame *outframe, AVFrame *inframe)
{
    int ret = 0;

    /* lazy filtergraph configuration: we need to wait for the first
     * frame to see what pixel format is getting decoded (no other way
     * with hardware acceleration apparently) */
    if (inframe) {
        TRACE(f, "input %s frame @ ts=%s",
            f->avctx->codec_type == AVMEDIA_TYPE_VIDEO ? av_get_pix_fmt_name(inframe->format)
                                                       : av_get_sample_fmt_name(inframe->format),
            PTS2TIMESTR(inframe->pts));

        // XXX: check width/height changes?
        if (f->last_frame_format != inframe->format) {
            f->last_frame_format = inframe->format;
            ret = setup_filtergraph(f);
            if (ret < 0)
                return ret;
        }
    }

    AVFrame *filtered_frame = f->avctx->codec_type == AVMEDIA_TYPE_AUDIO ? f->tmp_audio_frame : outframe;

    if (!f->filter_graph) {
        if (!inframe)
            return AVERROR_EOF;
        av_frame_move_ref(filtered_frame, inframe);
    } else {

        /* Push */
        if (inframe)
            TRACE(f, "push frame with ts=%s into filtergraph", PTS2TIMESTR(inframe->pts));
        else
            TRACE(f, "push null frame into filtergraph");

        ret = av_buffersrc_write_frame(f->buffersrc_ctx, inframe);
        if (ret < 0) {
            fprintf(stderr, "Error while feeding the filtergraph\n");
            return ret;
        }

        // not needed but make the buffer available again asap
        av_frame_unref(inframe);

        /* Pull */
        ret = av_buffersink_get_frame(f->buffersink_ctx, filtered_frame);
        TRACE(f, "got frame from sink ret=[%s]", av_err2str(ret));
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                fprintf(stderr, "Error while pulling the frame from the filtergraph\n");
        }
    }

    if (f->avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        audio_frame_to_sound_texture(f, f->audio_texture_frame, filtered_frame);
        av_frame_unref(filtered_frame);
        av_frame_ref(outframe, f->audio_texture_frame);
    }

    TRACE(f, "output %s frame @ ts=%s",
          av_get_pix_fmt_name(outframe->format), PTS2TIMESTR(outframe->pts));

    return ret;
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

int filtering_init(struct filtering_ctx *f,
                   AVThreadMessageQueue *in_queue,
                   AVThreadMessageQueue *out_queue,
                   int sw_pix_fmt,
                   const AVCodecContext *avctx)
{
    f->in_queue  = in_queue;
    f->out_queue = out_queue;
    f->sw_pix_fmt = sw_pix_fmt;

    f->avctx = avcodec_alloc_context3(NULL);
    if (!f->avctx)
        return AVERROR(ENOMEM);
    avcodec_copy_context(f->avctx, avctx);

    f->last_frame_format = AV_PIX_FMT_NONE;

    if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        f->audio_texture_frame = get_audio_frame();
        if (!f->audio_texture_frame)
            return AVERROR(ENOMEM);
        f->tmp_audio_frame = av_frame_alloc();
        if (!f->tmp_audio_frame)
            return AVERROR(ENOMEM);
    }

    f->filtered_frame = av_frame_alloc();
    if (!f->filtered_frame)
        return AVERROR(ENOMEM);

    if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        int i;

        /* Pre-calc windowing function */
        f->window_func_lut = av_malloc_array(AUDIO_NBSAMPLES, sizeof(*f->window_func_lut));
        if (!f->window_func_lut)
            return AVERROR(ENOMEM);
        for (i = 0; i < AUDIO_NBSAMPLES; i++)
            f->window_func_lut[i] = .5f * (1 - cos(2*M_PI*i / (AUDIO_NBSAMPLES-1)));

        /* Real Discrete Fourier Transform context (Real to Complex) */
        f->rdft = av_rdft_init(AUDIO_NBITS, DFT_R2C);
        if (!f->rdft) {
            fprintf(stderr, "Unable to init RDFT context with N=%d\n", AUDIO_NBITS);
            return AVERROR(ENOMEM);
        }

        f->rdft_data[0] = av_mallocz_array(AUDIO_NBSAMPLES, sizeof(*f->rdft_data[0]));
        f->rdft_data[1] = av_mallocz_array(AUDIO_NBSAMPLES, sizeof(*f->rdft_data[1]));
        if (!f->rdft_data[0] || !f->rdft_data[1])
            return AVERROR(ENOMEM);
    }

    return 0;
}

static int send_frame(struct filtering_ctx *f, AVFrame *frame)
{
    int ret;

    frame = av_frame_clone(frame); // XXX
    av_frame_unref(f->filtered_frame);
    if (!frame)
        return AVERROR(ENOMEM);

    TRACE(f, "sending filtered frame to the sink");
    ret = av_thread_message_queue_send(f->out_queue, &frame, 0);
    if (ret < 0)
        av_frame_free(&frame);
    return ret;
}

void filtering_run(struct filtering_ctx *f)
{
    int ret;

    for (;;) {
        AVFrame *frame;

        TRACE(f, "fetching a frame from the inqueue");
        ret = av_thread_message_queue_recv(f->in_queue, &frame, 0);
        if (ret < 0) {
            TRACE(f, "unable to fetch a frame from the inqueue: %s", av_err2str(ret));

            // Only valid reason to flush: when there is no packet remaining in
            // the input queue
            if (ret == AVERROR_EOF) {
                TRACE(f, "flush frames from filtergraph");
                for (;;) {
                    ret = filter_frame(f, f->filtered_frame, NULL);
                    if (ret < 0)
                        break;
                    ret = send_frame(f, f->filtered_frame);
                    if (ret < 0)
                        break;
                }
            }
            break;
        }

        // TODO: replace with a trim filter in libavfilter (check if hw accelerated
        // filters work)
        if (f->max_pts != AV_NOPTS_VALUE && frame->pts >= f->max_pts) {
            av_frame_free(&frame);
            TRACE(f, "reached trim duration");
            break;
        }

        TRACE(f, "filtering frame with ts=%s", PTS2TIMESTR(frame->pts));
        ret = filter_frame(f, f->filtered_frame, frame);
        av_frame_free(&frame);
        if (ret < 0) {
            TRACE(f, "unable to filter frame: %s", av_err2str(ret));
            break;
        }

        ret = send_frame(f, f->filtered_frame);
        if (ret < 0) {
            TRACE(f, "unable to send frame: %s", av_err2str(ret));
            break;
        }
    }

    if (!ret)
        ret = AVERROR_EOF;
    TRACE(f, "mark in & out queue with %s", av_err2str(ret));
    av_thread_message_queue_set_err_send(f->in_queue,  ret);
    av_thread_message_queue_set_err_recv(f->out_queue, ret);
}

void filtering_uninit(struct filtering_ctx *f)
{
    if (f->avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_frame_free(&f->audio_texture_frame);
        av_frame_free(&f->tmp_audio_frame);
        av_freep(&f->window_func_lut);
        av_freep(&f->rdft_data[0]);
        av_freep(&f->rdft_data[1]);
        if (f->rdft) {
            av_rdft_end(f->rdft);
            f->rdft = NULL;
        }
    }
    av_frame_free(&f->filtered_frame);
    avfilter_graph_free(&f->filter_graph);
    avcodec_free_context(&f->avctx);
}
