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
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/timestamp.h>

#include "sxplayer.h"
#include "internal.h"
#include "mod_filtering.h"
#include "log.h"
#include "msg.h"

#define AUDIO_NBITS      10
#define AUDIO_NBSAMPLES  (1<<(AUDIO_NBITS))
#define AUDIO_NBCHANNELS 2

struct filtering_ctx {
    void *log_ctx;

    AVThreadMessageQueue *in_queue;
    AVThreadMessageQueue *out_queue;

    AVCodecParameters *codecpar;
    char *filters;
    int64_t max_pts;
    int sw_pix_fmt;
    int max_pixels;
    int audio_texture;
    AVRational st_timebase;

    AVFilterGraph *filter_graph;
    enum AVPixelFormat last_frame_format;
    AVFilterContext *buffersink_ctx;        // sink of the graph (from where we pull)
    AVFilterContext *buffersrc_ctx;         // source of the graph (where we push)
    float *window_func_lut;                 // audio window function lookup table
    RDFTContext *rdft;                      // real discrete fourier transform context
    FFTSample *rdft_data[AUDIO_NBCHANNELS]; // real discrete fourier transform data for each channel
};

struct filtering_ctx *sxpi_filtering_alloc(void)
{
    struct filtering_ctx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->codecpar = avcodec_parameters_alloc();
    if (!ctx->codecpar) {
        av_freep(&ctx);
        return NULL;
    }
    return ctx;
}

/**
 * Convert an audio frame (PCM data) to a textured video frame with waves and
 * FFT lines
 */
static void audio_frame_to_sound_texture(struct filtering_ctx *ctx, AVFrame *dst_video,
                                         const AVFrame *audio_src)
{
    int i, j, ch;
    const int nb_samples = audio_src->nb_samples;
    const int width = nb_samples / 2;
    const float scale = 1.f / sqrt(AUDIO_NBSAMPLES/2 + 1);

    TRACE(ctx, "transform audio filtered frame in %s @ ts=%s into an audio texture",
          av_get_sample_fmt_name(audio_src->format),
          av_ts2timestr(audio_src->pts, &ctx->st_timebase));

    dst_video->pts = audio_src->pts;

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
        float *bins = ctx->rdft_data[ch];

        /* Apply window function to input samples */
        for (i = 0; i < nb_samples; i++)
            bins[i] = samples_src[i] * ctx->window_func_lut[i];

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
        av_rdft_calc(ctx->rdft, bins);

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

            TRACE(ctx, "line %2d->%2d: %3d different pixels (copied %3dx) as destination, step source: %d",
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
static int setup_filtergraph(struct filtering_ctx *ctx)
{
    int ret = 0;
    char args[512];
    AVFilter *buffersrc, *buffersink;
    AVFilterInOut *outputs, *inputs;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(ctx->last_frame_format);
    const AVCodecParameters *codecpar = ctx->codecpar;
    const AVRational time_base = ctx->st_timebase;

    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)
        return 0;

    avfilter_graph_free(&ctx->filter_graph);

    outputs = avfilter_inout_alloc();
    inputs  = avfilter_inout_alloc();

    buffersrc  = avfilter_get_by_name(codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "buffer" : "abuffer");
    buffersink = avfilter_get_by_name(codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? "buffersink" : "abuffersink");

    ctx->filter_graph = avfilter_graph_alloc();

    if (!inputs || !outputs || !ctx->filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    av_opt_set_int(ctx->filter_graph, "threads", 1, 0);

    inputs->name  = av_strdup("out");
    outputs->name = av_strdup("in");
    if (!inputs->name || !outputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* create buffer filter source (where we push the frame) */
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%s:time_base=%d/%d:pixel_aspect=%d/%d:sws_param=flags=bicubic",
                 codecpar->width, codecpar->height, av_get_pix_fmt_name(ctx->last_frame_format),
                 time_base.num, time_base.den,
                 codecpar->sample_aspect_ratio.num, codecpar->sample_aspect_ratio.den);
    } else {
        snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s",
                 time_base.num, time_base.den, codecpar->sample_rate,
                 av_get_sample_fmt_name(codecpar->format));
        if (codecpar->channel_layout)
            av_strlcatf(args, sizeof(args), ":channel_layout=0x%"PRIx64, codecpar->channel_layout);
        else
            av_strlcatf(args, sizeof(args), ":channels=%d", codecpar->channels);
    }

    TRACE(ctx, "graph buffer source args: %s", args);

    ret = avfilter_graph_create_filter(&ctx->buffersrc_ctx, buffersrc,
                                       outputs->name, args, NULL, ctx->filter_graph);
    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to create buffer filter source");
        goto end;
    }

    /* create buffer filter sink (where we pull the frame) */
    ret = avfilter_graph_create_filter(&ctx->buffersink_ctx, buffersink,
                                       inputs->name, NULL, NULL, ctx->filter_graph);
    if (ret < 0) {
        LOG(ctx, ERROR, "Unable to create buffer filter sink");
        goto end;
    }

    /* define the output of the graph */
    snprintf(args, sizeof(args), "%s", ctx->filters ? ctx->filters : "");
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(ctx->last_frame_format);
        const enum AVPixelFormat sw_pix_fmt = sxpi_pix_fmts_sx2ff(ctx->sw_pix_fmt);
        const enum AVPixelFormat pix_fmt = !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL) ? sw_pix_fmt : ctx->last_frame_format;

        if (ctx->max_pixels) {
            int w = codecpar->width, h = codecpar->height;
            sxpi_update_dimensions(&w, &h, ctx->max_pixels);
            av_strlcatf(args, sizeof(args),
                        "%sscale=%d:%d:force_original_aspect_ratio=decrease",
                        *args ? "," : "", w, h);
        }

        av_strlcatf(args, sizeof(args), "%sformat=%s, settb=tb=%d/%d", *args ? "," : "", av_get_pix_fmt_name(pix_fmt),
                    time_base.num, time_base.den);
    } else if (ctx->audio_texture) {
        av_strlcatf(args, sizeof(args), "aformat=sample_fmts=fltp:channel_layouts=stereo, asetnsamples=%d, asettb=tb=%d/%d",
                    AUDIO_NBSAMPLES, time_base.num, time_base.den);
    } else {
        av_strlcatf(args, sizeof(args), "%saformat=sample_fmts=flt:channel_layouts=stereo, asettb=tb=%d/%d",
                    *args ? "," : "", time_base.num, time_base.den);
    }

    TRACE(ctx, "graph buffer sink args: %s", args);

    /* create our filter graph */
    inputs->filter_ctx  = ctx->buffersink_ctx;
    outputs->filter_ctx = ctx->buffersrc_ctx;

    ret = avfilter_graph_parse_ptr(ctx->filter_graph, args, &inputs, &outputs, NULL);
    if (ret < 0)
        goto end;

    ret = avfilter_graph_config(ctx->filter_graph, NULL);
    if (ret < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
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

int sxpi_filtering_init(void *log_ctx,
                        struct filtering_ctx *ctx,
                        AVThreadMessageQueue *in_queue,
                        AVThreadMessageQueue *out_queue,
                        const AVStream *stream,
                        const AVCodecContext *avctx,
                        double media_rotation,
                        const struct sxplayer_opts *o)
{
    int ret;

    ctx->log_ctx = log_ctx;
    ctx->in_queue  = in_queue;
    ctx->out_queue = out_queue;
    ctx->sw_pix_fmt = o->sw_pix_fmt;
    ctx->max_pixels = o->max_pixels;
    ctx->audio_texture = o->audio_texture;
    ctx->st_timebase = stream->time_base;
    ctx->max_pts = o->trim_duration64 > 0 ? av_rescale_q(o->skip64 + o->trim_duration64, AV_TIME_BASE_Q, ctx->st_timebase)
                                          : AV_NOPTS_VALUE;

    ret = avcodec_parameters_from_context(ctx->codecpar, avctx);
    if (ret < 0)
        return ret;

    if (ctx->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_texture) {
        int i;

        /* Pre-calc windowing function */
        ctx->window_func_lut = av_malloc_array(AUDIO_NBSAMPLES, sizeof(*ctx->window_func_lut));
        if (!ctx->window_func_lut)
            return AVERROR(ENOMEM);
        for (i = 0; i < AUDIO_NBSAMPLES; i++)
            ctx->window_func_lut[i] = .5f * (1 - cos(2*M_PI*i / (AUDIO_NBSAMPLES-1)));

        /* Real Discrete Fourier Transform context (Real to Complex) */
        ctx->rdft = av_rdft_init(AUDIO_NBITS, DFT_R2C);
        if (!ctx->rdft) {
            LOG(ctx, ERROR, "Unable to init RDFT context with N=%d", AUDIO_NBITS);
            return AVERROR(ENOMEM);
        }

        ctx->rdft_data[0] = av_mallocz_array(AUDIO_NBSAMPLES, sizeof(*ctx->rdft_data[0]));
        ctx->rdft_data[1] = av_mallocz_array(AUDIO_NBSAMPLES, sizeof(*ctx->rdft_data[1]));
        if (!ctx->rdft_data[0] || !ctx->rdft_data[1])
            return AVERROR(ENOMEM);
    }

    if (o->filters) {
        ctx->filters = av_strdup(o->filters);
        if (!ctx->filters)
            return AVERROR(ENOMEM);
    }

    if (ctx->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && o->autorotate) {
        if (fabs(media_rotation - 90) < 1.0)
            ctx->filters = update_filters_str(ctx->filters, "transpose=clock");
        else if (fabs(media_rotation - 180) < 1.0)
            ctx->filters = update_filters_str(ctx->filters, "vflip,hflip");
        else if (fabs(media_rotation - 270) < 1.0)
            ctx->filters = update_filters_str(ctx->filters, "transpose=cclock");
        TRACE(ctx, "update filtergraph to: %s", ctx->filters);
    }

    return 0;
}

static int send_frame(struct filtering_ctx *ctx, AVFrame *frame)
{
    int ret;
    struct message msg = {
        .type = MSG_FRAME,
        .data = frame,
    };

    TRACE(ctx, "sending filtered frame to the sink");
    ret = av_thread_message_queue_send(ctx->out_queue, &msg, 0);
    if (ret < 0) {
        if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
            LOG(ctx, ERROR, "unable to send frame: %s", av_err2str(ret));
    }

    return ret;
}

static int push_frame(struct filtering_ctx *ctx, AVFrame *inframe)
{
    int ret;

    TRACE(ctx, "pushing frame %p into filtergraph", inframe);

    ret = av_buffersrc_write_frame(ctx->buffersrc_ctx, inframe);
    if (ret < 0) {
        LOG(ctx, ERROR, "unable to push frame into filtergraph: %s", av_err2str(ret));
        return ret;
    }

    return 0;
}

static int pull_frame(struct filtering_ctx *ctx, AVFrame *outframe)
{
    int ret;
    const int do_audio_texture = ctx->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_texture;
    AVFrame *filtered_frame = outframe;

    TRACE(ctx, "pulling frame from filtergraph");

    if (do_audio_texture) {
        filtered_frame = av_frame_alloc();
        if (!filtered_frame)
            return AVERROR(ENOMEM);
    }

    ret = av_buffersink_get_frame(ctx->buffersink_ctx, filtered_frame);
    if (ret < 0) {
        if (do_audio_texture)
            av_frame_free(&filtered_frame);
        if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN))
            LOG(ctx, ERROR, "unable to pull frame from filtergraph: %s", av_err2str(ret));
        return ret;
    }

    TRACE(ctx, "filtered %s %s frame @ ts=%s",
          av_get_media_type_string(ctx->codecpar->codec_type),
          ctx->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? av_get_pix_fmt_name(filtered_frame->format)
                                                          : av_get_sample_fmt_name(filtered_frame->format),
          av_ts2timestr(filtered_frame->pts, &ctx->st_timebase));

    if (do_audio_texture) {
        AVFrame *audio_texture_frame = get_audio_frame();
        audio_frame_to_sound_texture(ctx, audio_texture_frame, filtered_frame);
        av_frame_free(&filtered_frame);
        av_frame_move_ref(outframe, audio_texture_frame);
        av_free(audio_texture_frame);
    }

    return 0;
}

static int pull_send_frame(struct filtering_ctx *ctx)
{
    int ret;

    AVFrame *filtered_frame = av_frame_alloc();
    if (!filtered_frame)
        return AVERROR(ENOMEM);

    ret = pull_frame(ctx, filtered_frame);

    if (ret < 0) {
        av_frame_free(&filtered_frame);
        return ret;
    }

    ret = send_frame(ctx, filtered_frame);
    if (ret < 0) {
        av_frame_free(&filtered_frame);
        return ret;
    }

    return 0;
}

static int flush_frames(struct filtering_ctx *ctx)
{
    int ret;

    if (!ctx->filter_graph)
        return 0;

    TRACE(ctx, "push null frame into %s filtergraph to trigger flushing",
          av_get_media_type_string(ctx->codecpar->codec_type));

    ret = push_frame(ctx, NULL);
    if (ret < 0)
        return ret;

    do {
        ret = pull_send_frame(ctx);
    } while (ret >= 0);

    return ret;
}

void sxpi_filtering_run(struct filtering_ctx *ctx)
{
    int ret;
    int in_err, out_err;

    TRACE(ctx, "filtering packets from %p into %p", ctx->in_queue, ctx->out_queue);

    // we want to force the reconstruction of the filtergraph
    ctx->last_frame_format = AV_PIX_FMT_NONE;

    for (;;) {
        AVFrame *frame;
        struct message msg;

        TRACE(ctx, "fetching a frame from the inqueue");
        ret = av_thread_message_queue_recv(ctx->in_queue, &msg, 0);
        if (ret < 0) {
            if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
                LOG(ctx, ERROR, "unable to fetch a frame from the inqueue: %s", av_err2str(ret));
            break;
        }

        if (msg.type == MSG_SEEK) {
            TRACE(ctx, "message is a seek, destroy filtergraph and forward message to out queue");
            avfilter_graph_free(&ctx->filter_graph);
            ctx->last_frame_format = AV_PIX_FMT_NONE;
            av_thread_message_flush(ctx->out_queue);
            ret = av_thread_message_queue_send(ctx->out_queue, &msg, 0);
            if (ret < 0) {
                sxpi_msg_free_data(&msg);
                break;
            }
            continue;
        }

        frame = msg.data;

        TRACE(ctx, "filtering %s %s frame @ ts=%s",
              av_get_media_type_string(ctx->codecpar->codec_type),
              ctx->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? av_get_pix_fmt_name(frame->format)
                                                              : av_get_sample_fmt_name(frame->format),
              av_ts2timestr(frame->pts, &ctx->st_timebase));

        /* lazy filtergraph configuration */
        // XXX: check width/height/samplerate/etc changes?
        if (ctx->last_frame_format != frame->format) {
            ctx->last_frame_format = frame->format;
            ret = setup_filtergraph(ctx);
            if (ret < 0)
                break;
        }

        // TODO: replace with a trim filter in libavfilter (check if hw accelerated
        // filters work)
        if (frame->pts < 0) {
            av_frame_free(&frame);
            TRACE(ctx, "frame ts is negative, skipping");
            continue;
        } else if (ctx->max_pts != AV_NOPTS_VALUE && frame->pts >= ctx->max_pts) {
            av_frame_free(&frame);
            TRACE(ctx, "reached trim duration");
            ret = AVERROR_EXIT; // not EOF because we do not want to flush the frames
            break;
        }

        if (!ctx->filter_graph) {
            ret = send_frame(ctx, frame);
            if (ret < 0) {
                av_frame_free(&frame);
                break;
            }
        } else {
            ret = push_frame(ctx, frame);
            av_frame_free(&frame);
            if (ret < 0)
                break;

            ret = pull_send_frame(ctx);
            if (ret < 0 && ret != AVERROR(EAGAIN))
                break;
        }
    }

    /* Fetch remaining frames */
    if (ret == AVERROR_EOF)
        ret = flush_frames(ctx);

    if (ret < 0 && ret != AVERROR_EOF) {
        in_err = out_err = ret;
    } else {
        in_err = AVERROR_EXIT;
        out_err = AVERROR_EOF;
    }
    TRACE(ctx, "notify decoder with %s and sink with %s",
          av_err2str(in_err), av_err2str(out_err));
    av_thread_message_queue_set_err_send(ctx->in_queue,  in_err);
    av_thread_message_flush(ctx->in_queue);
    av_thread_message_queue_set_err_recv(ctx->out_queue, out_err);
}

void sxpi_filtering_free(struct filtering_ctx **fp)
{
    struct filtering_ctx *ctx = *fp;

    if (!ctx)
        return;

    if (ctx->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_texture) {
        av_freep(&ctx->window_func_lut);
        av_freep(&ctx->rdft_data[0]);
        av_freep(&ctx->rdft_data[1]);
        if (ctx->rdft) {
            av_rdft_end(ctx->rdft);
            ctx->rdft = NULL;
        }
    }
    avfilter_graph_free(&ctx->filter_graph);
    avcodec_parameters_free(&ctx->codecpar);
    av_freep(&ctx->filters);
    av_freep(fp);
}
