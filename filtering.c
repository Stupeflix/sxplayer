#include <libavcodec/avfft.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avstring.h>
#include <libavutil/pixdesc.h>
#include <libavutil/timestamp.h>

#include "sxplayer.h"
#include "internal.h"
#include "filtering.h"

// FIXME
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

int push_frame_cb(void *priv, AVFrame *frame)
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
