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
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>

#include "sfxmp.h"

struct Frame {
    AVFrame *frame;         // a frame the user might want
    double ts;              // rescaled timestamp
};

#define AUDIO_NBITS      10
#define AUDIO_NBSAMPLES  (1<<(AUDIO_NBITS))
#define AUDIO_NBCHANNELS 2

struct sfxmp_ctx {
    /* configurable options */
    char *filename;                         // input filename
    int avselect;                           // select audio or video
    double visible_time;                    // see public header
    double start_time;                      // see public header
    double skip;                            // see public header
    double trim_duration;                   // see public header
    int max_nb_frames;                      // maximum number of frames in the queue
    double dist_time_seek_trigger;          // distance time triggering a seek

    /* misc general fields */
    enum AVMediaType media_type;            // AVMEDIA_TYPE_{VIDEO,AUDIO} according to avselect
    const char *media_type_string;          // "audio" or "video" according to avselect

    /* main vs demuxer/decoder thread negotiation */
    pthread_t dec_thread;                   // decoding thread
    pthread_mutex_t queue_lock;             // for any op related to the queue
    pthread_cond_t queue_reduce;            // notify a reducing queue (MUST be ONLY signaled from main thread)
    pthread_cond_t queue_grow;              // notify a growing queue (MUST be ONLY signaled from decoding thread)

    /* fields that MUST be protected by queue_lock */
    int queue_terminated;                   // 0 if queue is not going to be filled anymore (because thread is dead), 1 otherwise
    struct Frame *frames;                   // queue of the decoded (and filtered) frames
    int nb_frames;                          // total number of frames in the queue
    double request_seek;                    // field used by the main thread to request a seek to the decoding thread
    int can_seek_again;                     // field used to avoid seeking again until the requested time is reached

    /* fields specific to main thread */
    struct Frame non_visible;               // frame to display for the "non visible" time zone
    struct sfxmp_frame rframe;              // user returned frame container
    double last_pushed_frame_ts;            // ts value of the latest pushed frame (it acts as a UID)

    /* fields specific to decoding thread */
    AVFrame *decoded_frame;                 // decoded frame
    AVFrame *filtered_frame;                // filtered version of decoded_frame
    AVFrame *audio_texture_frame;           // wave/fft texture in case of audio
    AVFormatContext *fmt_ctx;               // demuxing context
    AVCodecContext  *dec_ctx;               // decoder context
    AVStream *stream;                       // selected stream
    int stream_idx;                         // selected stream index
    AVFilterGraph *filter_graph;            // libavfilter graph
    char *filter_graph_str;                 // libavfilter graph string
    AVFilterContext *buffersink_ctx;        // sink of the graph (from where we pull)
    AVFilterContext *buffersrc_ctx;         // source of the graph (where we push)
    float *window_func_lut;                 // audio window function lookup table
    RDFTContext *rdft;                      // real discrete fourier transform context
    FFTSample *rdft_data[AUDIO_NBCHANNELS]; // real discrete fourier transform data for each channel
};

#define ENABLE_DBG 0

#define DBG_SFXMP(mod, ...) do { printf("[sfxmp:"mod"] " __VA_ARGS__); fflush(stdout); } while (0)
#if ENABLE_DBG
# define DBG(mod, ...) DBG_SFXMP(mod, __VA_ARGS__)
#else
# define DBG(mod, ...) do { if (0) DBG_SFXMP(mod, __VA_ARGS__); } while (0)
#endif

/**
 * Allocate a small frame to be displayed before visible_time
 */
static AVFrame *get_invisible_frame(enum AVMediaType media_type)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return NULL;

    frame->format = AV_PIX_FMT_RGB32;

    if (media_type == AVMEDIA_TYPE_VIDEO) {
        frame->width  = 2;
        frame->height = 2;
    } else {
        frame->width  = AUDIO_NBSAMPLES/2;      // samples are float (32 bits), pix fmt is rgb32 (32 bits as well)
        /* height:
         *   AUDIO_NBCHANNELS (waves lines)
         * + AUDIO_NBCHANNELS (fft lines of width AUDIO_NBCHANNELS/2, or 1<<(AUDIO_NBITS-1))
         * + AUDIO_NBITS-1 AUDIO_NBCHANNELS (fft lines downscaled) */
        frame->height = (1 + AUDIO_NBITS) * AUDIO_NBCHANNELS;
    }

    if (av_frame_get_buffer(frame, 16) < 0) {
        av_frame_free(&frame);
        return NULL;
    }

#define SET_COLOR(x, y, color) *(uint32_t *)&frame->data[0][y*frame->linesize[0] + x*4] = color

    if (media_type == AVMEDIA_TYPE_VIDEO) {
#if ENABLE_DBG
    /* In debug more, we make them colored for visual debug */
    SET_COLOR(0, 0, 0xff0000ff);
    SET_COLOR(1, 1, 0x00ff00ff);
    SET_COLOR(0, 1, 0x0000ffff);
    SET_COLOR(1, 0, 0xffffffff);
#else
    /* ...but in production we want it to be black */
    SET_COLOR(0, 0, 0x00000000);
    SET_COLOR(1, 1, 0x00000000);
    SET_COLOR(0, 1, 0x00000000);
    SET_COLOR(1, 0, 0x00000000);
#endif
    } else {
        memset(frame->data[0], 0, frame->height * frame->linesize[0]);
    }
    return frame;
}

static void free_context(struct sfxmp_ctx *s)
{
    int i;

    if (!s)
        return;

    if (s->frames) {
        for (i = 0; i < s->max_nb_frames; i++)
            av_frame_free(&s->frames[i].frame);
        av_freep(&s->frames);
    }

    av_frame_free(&s->non_visible.frame);
    av_freep(&s->filename);
    av_freep(&s->filter_graph_str);
    av_freep(&s);
}

static char *get_filter_graph_str(enum AVMediaType media_type, const char *filters)
{
    char buf[512];

    if (media_type == AVMEDIA_TYPE_VIDEO)
        snprintf(buf, sizeof(buf), "format=rgb32");
    else
        snprintf(buf, sizeof(buf), "aformat=sample_fmts=fltp:channel_layouts=stereo, asetnsamples=%d", AUDIO_NBSAMPLES);

    return filters ? av_asprintf("%s,%s", filters, buf)
                   : av_strdup(buf);
}

struct sfxmp_ctx *sfxmp_create(const char *filename,
                               int avselect,
                               double visible_time,
                               double start_time,
                               double skip,
                               double trim_duration,
                               double dist_time_seek_trigger,
                               double max_nb_frames,
                               const char *filters)
{
    int i;
    struct sfxmp_ctx *s;

    av_register_all();
    avfilter_register_all();
    av_log_set_level(AV_LOG_ERROR);

    s = av_mallocz(sizeof(*s));
    if (!s)
        return NULL;

    s->filename               = av_strdup(filename);
    s->avselect               = avselect;
    s->visible_time           = visible_time;
    s->start_time             = start_time;
    s->skip                   = skip;
    s->trim_duration          = trim_duration;
    s->dist_time_seek_trigger = dist_time_seek_trigger < 0 ? 3 : dist_time_seek_trigger;
    s->max_nb_frames          = max_nb_frames < 0 ? 5 : max_nb_frames;

    switch (s->avselect) {
    case SFXMP_SELECT_VIDEO: s->media_type = AVMEDIA_TYPE_VIDEO; break;
    case SFXMP_SELECT_AUDIO: s->media_type = AVMEDIA_TYPE_AUDIO; break;
    default:
        fprintf(stderr, "unknown avselect value %d\n", s->avselect);
        goto fail;
    }

    s->media_type_string = av_get_media_type_string(s->media_type);
    av_assert0(s->media_type_string);

    s->filter_graph_str = get_filter_graph_str(s->media_type, filters);

    if (s->max_nb_frames < 2) {
        fprintf(stderr, "max_nb_frames < 2 is not supported\n");
        goto fail;
    }

    if (!s->filename || !s->filter_graph_str)
        goto fail;

    s->frames = av_malloc_array(s->max_nb_frames, sizeof(*s->frames));
    if (!s->frames)
        goto fail;
    for (i = 0; i < s->max_nb_frames; i++) {
        s->frames[i].frame = av_frame_alloc();
        if (!s->frames[i].frame)
            goto fail;
    }

    s->last_pushed_frame_ts = DBL_MIN;

    s->non_visible.ts    = -1;
    s->non_visible.frame = get_invisible_frame(s->media_type);
    if (!s->non_visible.frame)
        goto fail;

    s->queue_terminated = 1;

    pthread_mutex_init(&s->queue_lock, NULL);
    pthread_cond_init(&s->queue_reduce, NULL);
    pthread_cond_init(&s->queue_grow, NULL);

    DBG("init", "filename:%s avselect:%d visible_time:%f start_time:%f skip:%f trim_duration:%f (%p)\n",
        filename, avselect, visible_time, start_time, skip, trim_duration, s);

    return s;

fail:
    free_context(s);
    return NULL;
}

void sfxmp_free(struct sfxmp_ctx **ss)
{
    struct sfxmp_ctx *s = *ss;

    DBG("free", "calling sfxmp_free() (%p)\n", s);

    if (!s)
        return;

    if (!s->queue_terminated) {
        DBG("free", "queue is not terminated yet\n");
        pthread_mutex_lock(&s->queue_lock);
        s->queue_terminated = 1; // notify decoding thread must die
        pthread_mutex_unlock(&s->queue_lock);
        pthread_cond_signal(&s->queue_reduce); // wake up decoding thread

        pthread_join(s->dec_thread, NULL);
    }
    pthread_cond_destroy(&s->queue_reduce);
    pthread_cond_destroy(&s->queue_grow);
    pthread_mutex_destroy(&s->queue_lock);
    free_context(s);
}

static int decode_packet(struct sfxmp_ctx *s, AVPacket *pkt,
                         AVFrame *frame, int *got_frame)
{
    int decoded = pkt->size;

    *got_frame = 0;
    if (pkt->stream_index == s->stream_idx) {
        int ret;

        if (s->media_type == AVMEDIA_TYPE_VIDEO)
            ret = avcodec_decode_video2(s->dec_ctx, frame, got_frame, pkt);
        else
            ret = avcodec_decode_audio4(s->dec_ctx, frame, got_frame, pkt);

        if (ret < 0) {
            fprintf(stderr, "Error decoding %s frame\n", s->media_type_string);
            return ret;
        }
        decoded = FFMIN(ret, pkt->size);
    }
    return decoded;
}

static int open_ifile(struct sfxmp_ctx *s, const char *infile)
{
    int ret = 0;
    AVCodec *dec;
    AVDictionary *opts = NULL;
    AVInputFormat *ifmt = NULL;

    ret = avformat_open_input(&s->fmt_ctx, infile, ifmt, &opts);
    if (ret < 0) {
        fprintf(stderr, "Unable to open input file '%s'\n", infile);
        return ret;
    }

    ret = avformat_find_stream_info(s->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Unable to find input stream information\n");
        return ret;
    }

    ret = av_find_best_stream(s->fmt_ctx, s->media_type, -1, -1, &dec, 0);
    if (ret < 0) {
        fprintf(stderr, "Unable to find a %s stream in the input file\n", s->media_type_string);
        return ret;
    }
    s->stream_idx = ret;
    s->stream = s->fmt_ctx->streams[s->stream_idx];
    s->dec_ctx = s->stream->codec;
    av_opt_set_int(s->dec_ctx, "refcounted_frames", 1, 0);

    ret = avcodec_open2(s->dec_ctx, dec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Unable to open input %s decoder\n", s->media_type_string);
        return ret;
    }

    /* If trim_duration is not specified, we try to probe from the stream or
     * format (presentation) duration */
    if (s->trim_duration < 0) {
        int64_t duration = s->fmt_ctx->duration;
        double scale = av_q2d(AV_TIME_BASE_Q);

        if (duration == AV_NOPTS_VALUE) {
            duration = s->stream->duration;
            if (s->stream->time_base.den)
                scale = av_q2d(s->stream->time_base);
            else
                duration = AV_NOPTS_VALUE;
        }
        if (duration == AV_NOPTS_VALUE) {
            fprintf(stderr, "trim_duration is not set and can't estimate the duration, aborting\n");
            return AVERROR_INVALIDDATA;
        }
        s->trim_duration = duration * scale;
        DBG("decoder", "set trim duration to %f\n", s->trim_duration);
    }

    return 0;
}

/**
 * Map the timeline time to the media time
 */
static double get_media_time(const struct sfxmp_ctx *s, double t)
{
    return s->skip + av_clipd(t - s->start_time, 0, s->trim_duration);
}

/**
 * Request a seek to the demuxer
 */
static int seek_to(struct sfxmp_ctx *s, double t)
{
    const double vt = get_media_time(s, t);
    const int64_t ts = vt * AV_TIME_BASE;

    if (ENABLE_DBG)
        DBG("decoder", "Seek in media at %f (t=%f)\n", vt, t);
    else
        printf("Seek in media at %f (t=%f)\n", vt, t);
    return avformat_seek_file(s->fmt_ctx, -1, INT64_MIN, ts, ts, 0);
}

/**
 * Setup the libavfilter filtergraph for user filter but also to have a way to
 * request a pixel format we want, and let libavfilter insert the necessary
 * scaling filter (typically, an automatic conversion from yuv420p to rgb32).
 */
static int setup_filtergraph(struct sfxmp_ctx *s)
{
    int ret = 0;
    char args[512];
    const AVRational time_base = s->stream->time_base;
    const AVRational framerate = av_guess_frame_rate(s->fmt_ctx, s->stream, NULL);
    AVFilter *buffersrc  = avfilter_get_by_name(s->media_type == AVMEDIA_TYPE_VIDEO ? "buffer" : "abuffer");
    AVFilter *buffersink = avfilter_get_by_name(s->media_type == AVMEDIA_TYPE_VIDEO ? "buffersink" : "abuffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();

    avfilter_graph_free(&s->filter_graph);
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
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:sws_param=flags=bicubic",
                 s->dec_ctx->width, s->dec_ctx->height, s->dec_ctx->pix_fmt,
                 time_base.num, time_base.den,
                 s->dec_ctx->sample_aspect_ratio.num, s->dec_ctx->sample_aspect_ratio.den);
    } else {
        snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s",
                 1, s->dec_ctx->sample_rate, s->dec_ctx->sample_rate,
                 av_get_sample_fmt_name(s->dec_ctx->sample_fmt));
        if (s->dec_ctx->channel_layout)
            av_strlcatf(args, sizeof(args), ":channel_layout=0x%"PRIx64, s->dec_ctx->channel_layout);
        else
            av_strlcatf(args, sizeof(args), ":channels=%d", s->dec_ctx->channels);
    }

    if (framerate.num && framerate.den)
        av_strlcatf(args, sizeof(args), ":frame_rate=%d/%d", framerate.num, framerate.den);

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

    /* create our filter graph */
    inputs->filter_ctx  = s->buffersink_ctx;
    outputs->filter_ctx = s->buffersrc_ctx;
    ret = avfilter_graph_parse_ptr(s->filter_graph, s->filter_graph_str,
                                   &inputs, &outputs, NULL);
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

/**
 * Convert an audio frame (PCM data) to a textured video frame with waves and
 * FFT lines
 */
static void audio_frame_to_sound_texture(struct sfxmp_ctx *s, AVFrame *dst_video,
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

        /* Run transform */
        av_rdft_calc(s->rdft, bins);

        /* Get magnitude of frequency bins and copy result into texture */
#define MAGNITUDE(re, im) sqrtf(((re)*(re) + (im)*(im)) * scale)
        for (i = 0; i < width - 1; i++)
            fft_dst[i] = MAGNITUDE(bins[2*(i + 1)], bins[2*(i + 1) + 1]);
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

                DBG("audio2tex", "line %2d->%2d: %3d different pixels (copied %3dx) as destination, step source: %d\n",
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

static int64_t get_best_effort_ts(const AVFrame *f)
{
    const int64_t t = av_frame_get_best_effort_timestamp(f);
    return t != AV_NOPTS_VALUE ? t : f->pts;
}

/**
 * Filter and queue frame(s)
 */
static int queue_frame(struct sfxmp_ctx *s, AVFrame *inframe, AVPacket *pkt)
{
    int ret = 0;

    if (inframe) {
        inframe->pts = get_best_effort_ts(inframe);

        DBG("decoder", "push frame with ts=%s into filtergraph\n", av_ts2str(inframe->pts));

        /* push the decoded frame into the filtergraph */
        ret = av_buffersrc_write_frame(s->buffersrc_ctx, inframe);
        if (ret < 0) {
            fprintf(stderr, "Error while feeding the filtergraph\n");
            goto end;
        }

        /* the frame is sent into the filtergraph, we don't need it anymore */
        av_frame_unref(inframe);
    }

    for (;;) {
        struct Frame *f;
        int64_t ts;
        double rescaled_ts;

        /* try to get a frame from the filergraph */
        ret = av_buffersink_get_frame(s->buffersink_ctx, s->filtered_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            fprintf(stderr, "Error while pulling the frame from the filtergraph\n");
            goto end;
        }

        DBG("decoder", "decoded frame @ ts=%s %"PRId64"\n",
            av_ts2str(get_best_effort_ts(s->filtered_frame)), s->filtered_frame->pts);

        /* if audio, get the audio texture from the filtered audio frame */
        if (s->media_type == AVMEDIA_TYPE_AUDIO)
            audio_frame_to_sound_texture(s, s->audio_texture_frame, s->filtered_frame);

        /* we have a frame, wait for the queue to be ready for queuing it */
        DBG("decoder", "locking\n");
        pthread_mutex_lock(&s->queue_lock);
        DBG("decoder", "mutex acquired\n");
        while (s->nb_frames == s->max_nb_frames && !s->queue_terminated) {
            DBG("decoder", "waiting reduce\n");
            pthread_cond_wait(&s->queue_reduce, &s->queue_lock);
        }

        DBG("decoder", "let's decode\n");

        /* kill requested */
        if (s->queue_terminated) {
            DBG("decoder", "Kill requested in decoding thread\n");
            ret = AVERROR_EXIT;
            pthread_mutex_unlock(&s->queue_lock);
            av_frame_unref(s->filtered_frame);
            goto end;
        }

        /* a seek was requested by the main thread */
        if (s->request_seek != -1 && s->can_seek_again) {
            int i;
            for (i = 0; i < s->nb_frames; i++)
                av_frame_unref(s->frames[i].frame);
            s->nb_frames = 0;
            pthread_mutex_unlock(&s->queue_lock);

            s->can_seek_again = 0;

            ret = seek_to(s, s->request_seek);
            if (ret < 0) {
                fprintf(stderr, "unable to seek at %.3f, abort decoding thread\n", s->request_seek);
                ret = AVERROR_EXIT;
                av_frame_unref(s->filtered_frame);
                goto end;
            }
            DBG("decoder", "seek OK (ret=%d)\n", ret);

            /* flush decoder and filters; we can do this after the unlock since
             * only this thread is decoding and pushing frames in the
             * filtergraph anyway */
            if (pkt)
                pkt->size = 0;
            avcodec_flush_buffers(s->dec_ctx);
            av_frame_unref(s->filtered_frame);

            ret = setup_filtergraph(s);
            if (ret < 0)
                goto end;

            return 0;
        }

        /* request the end of any further queuing if the last frame added was
         * already a potential last frame */
        if (s->nb_frames > 0 && s->frames[0].ts >= s->skip + s->trim_duration) {
            DBG("decoder", "Reached trim duration in the decoding thread, request end\n");
            ret = AVERROR_EXIT;
            pthread_mutex_unlock(&s->queue_lock);
            av_frame_unref(s->filtered_frame);
            goto end;
        }

        /* interpolate (if needed) and rescale timestamp */
        ts = get_best_effort_ts(s->filtered_frame);
        if (ts == AV_NOPTS_VALUE) {
            DBG("decoder", "need TS interpolation\n");
            if (s->nb_frames) {
                AVRational frame_rate = av_guess_frame_rate(s->fmt_ctx, s->stream, s->filtered_frame);
                rescaled_ts = s->frames[s->nb_frames - 1].ts + 1./av_q2d(frame_rate);
            } else {
                DBG("decoder", "Can not interpolate timing, skip frame :(\n");
                pthread_mutex_unlock(&s->queue_lock);
                av_frame_unref(s->filtered_frame);
                continue;
            }
        } else {
            rescaled_ts = ts * av_q2d(s->stream->time_base);
        }

        /* finally queue the frame */
        f = &s->frames[s->nb_frames++];
        if (s->media_type == AVMEDIA_TYPE_VIDEO) {
            av_frame_move_ref(f->frame, s->filtered_frame);
        } else {
            av_frame_ref(f->frame, s->audio_texture_frame);
            av_frame_unref(s->filtered_frame);
        }
        f->ts = rescaled_ts;
        DBG("decoder", "queuing frame %2d/%d @ ts=%f\n", s->nb_frames, s->max_nb_frames, f->ts);

        /* we haven't reach the time requested yet, skipping frame */
        if (s->request_seek != -1) {
            if (rescaled_ts < get_media_time(s, s->request_seek)) {
                DBG("decoder", "Request seek (%f) not reached yet: %f<%f, skip frame\n",
                    s->request_seek, rescaled_ts, get_media_time(s, s->request_seek));
            } else {
                /* Sometimes, because of inaccuracies with floats (or
                 * eventually a bug in FFmpeg), the first frame picked after a
                 * seek will be slightly off, creating a negative diff in
                 * the main loop, which will as a result cause the main thread
                 * to request a backseek, ended up in an infinite and
                 * undesirable seek loop.
                 * In order to avoid this, we lie about the timestamp and make
                 * it exactly what the user requested. */
                f->ts = get_media_time(s, s->request_seek);
                s->can_seek_again = 1;
                s->request_seek   = -1;
            }
        }

        /* frame is completely queued, release lock */
        DBG("decoder", "unlock\n");
        pthread_mutex_unlock(&s->queue_lock);

        /* signal the main thread that the queue got a new frame */
        pthread_cond_signal(&s->queue_grow);
    }

end:
    return ret;
}

static void *decoder_thread(void *arg)
{
    int ret, got_frame;
    AVPacket pkt = {0};
    struct sfxmp_ctx *s = arg;

    ret = open_ifile(s, s->filename);
    if (ret < 0)
        goto end;

    ret = setup_filtergraph(s);
    if (ret < 0)
        goto end;

    s->filtered_frame = av_frame_alloc();
    s->decoded_frame  = av_frame_alloc();
    if (!s->decoded_frame || !s->filtered_frame)
        goto end;

    if (s->media_type == AVMEDIA_TYPE_AUDIO) {
        s->audio_texture_frame = get_invisible_frame(AVMEDIA_TYPE_AUDIO);
        if (!s->audio_texture_frame)
            goto end;
    }

    if (s->media_type == AVMEDIA_TYPE_AUDIO) {
        int i;

        /* Pre-calc windowing function */
        s->window_func_lut = av_malloc_array(AUDIO_NBSAMPLES, sizeof(*s->window_func_lut));
        if (!s->window_func_lut)
            goto end;
        for (i = 0; i < AUDIO_NBSAMPLES; i++)
            s->window_func_lut[i] = .5f * (1 - cos(2*M_PI*i / (AUDIO_NBSAMPLES-1)));

        /* Real Discrete Fourier Transform context (Real to Complex) */
        s->rdft = av_rdft_init(AUDIO_NBITS, DFT_R2C);
        if (!s->rdft) {
            fprintf(stderr, "Unable to init RDFT context with N=%d\n", AUDIO_NBITS);
            goto end;
        }

        s->rdft_data[0] = av_mallocz_array(AUDIO_NBSAMPLES, sizeof(*s->rdft_data[0]));
        s->rdft_data[1] = av_mallocz_array(AUDIO_NBSAMPLES, sizeof(*s->rdft_data[1]));
        if (!s->rdft_data[0] || !s->rdft_data[1])
            goto end;
    }

    av_init_packet(&pkt);

    /* read frames from the file */
    while (av_read_frame(s->fmt_ctx, &pkt) >= 0) {
        AVPacket orig_pkt = pkt;
        do {
            ret = decode_packet(s, &pkt, s->decoded_frame, &got_frame);
            if (ret < 0)
                break;
            pkt.data += ret;
            pkt.size -= ret;
            if (got_frame) {
                ret = queue_frame(s, s->decoded_frame, &pkt);
                if (ret == AVERROR_EXIT) {
                    av_free_packet(&orig_pkt);
                    goto end;
                }
            }
        } while (pkt.size > 0);
        av_free_packet(&orig_pkt);
    }

    /* flush cached frames */
    pkt.data = NULL;
    pkt.size = 0;
    do {
        ret = decode_packet(s, &pkt, s->decoded_frame, &got_frame);
        if (ret == 0 && got_frame)
            queue_frame(s, s->decoded_frame, NULL);
    } while (got_frame);

    /* flush filtergraph */
    ret = av_buffersrc_write_frame(s->buffersrc_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error sending EOF in filtergraph\n");
        goto end;
    }
    do {
        ret = queue_frame(s, NULL, NULL);
    } while (ret >= 0);

end:
    if (s->fmt_ctx) {
        avcodec_close(s->dec_ctx);
        avformat_close_input(&s->fmt_ctx);
    }
    av_frame_free(&s->decoded_frame);
    av_frame_free(&s->filtered_frame);
    avfilter_graph_free(&s->filter_graph);

    if (s->media_type == AVMEDIA_TYPE_AUDIO) {
        av_frame_free(&s->audio_texture_frame);
        av_freep(&s->window_func_lut);
        av_freep(&s->rdft_data[0]);
        av_freep(&s->rdft_data[1]);
        if (s->rdft) {
            av_rdft_end(s->rdft);
            s->rdft = NULL;
        }
    }

    pthread_mutex_lock(&s->queue_lock);
    s->queue_terminated = 1;
    pthread_mutex_unlock(&s->queue_lock);
    pthread_cond_signal(&s->queue_grow);
    DBG("decoder", "decoding thread ends\n");
    return NULL;
}

/* Return the frame only if different from previous one. We do not make a
 * simple pointer check because of the frame reference counting (and thus
 * pointer reuse, depending on many parameters)  */
static const struct sfxmp_frame *ret_frame(struct sfxmp_ctx *s, const struct Frame *frame)
{
    const AVFrame *rframe = frame->frame;

    /* if same frame as previously, do not raise it again */
    if (s->last_pushed_frame_ts == frame->ts) {
        DBG("main", "same frame as previously, return NULL\n");
        return NULL;
    } else {
        DBG("main", "last_pushed_frame_ts:%f frame_ts:%f\n",
            s->last_pushed_frame_ts, frame->ts);
    }

    s->last_pushed_frame_ts = frame->ts;

    s->rframe.data     = rframe->data[0];
    s->rframe.linesize = rframe->linesize[0];
    s->rframe.width    = rframe->width;
    s->rframe.height   = rframe->height;
    s->rframe.ts       = frame->ts;
    return &s->rframe;
}

/**
 * return positive value => need to decode further
 * return negative value => need to seek back
 */
static double get_frame_dist(const struct sfxmp_ctx *s, int i, double t)
{
    const double frame_ts = s->frames[i].ts;
    const double req_frame_ts = get_media_time(s, t);
    const double dist = req_frame_ts - frame_ts;
    DBG("main", "frame[%2d/%2d]: %p t:%f ts:%f req:%f -> dist:%f\n",
        i+1, s->nb_frames, s->frames[i].frame, t, frame_ts, req_frame_ts, dist);
    return dist;
}

static int consume_queue(struct sfxmp_ctx *s, int n)
{
    int i;

    if (n < 1)
        return 0;

    DBG("main", "queue: drop %d/%d frame%s\n", n, s->nb_frames, n > 1 ? "s" : "");
    for (i = 0; i < n; i++)
        av_frame_unref(s->frames[i].frame);
    for (i = n; i < s->nb_frames; i++) {
        struct Frame *dstf = &s->frames[i - n];
        struct Frame *srcf = &s->frames[i];

        DBG("main", "queue: move queue[%d] %p (data:%p) -> queue[%d] %p\n",
            i, srcf->frame, srcf->frame->data[0], i - n, dstf->frame);
        av_frame_move_ref(dstf->frame, srcf->frame);
        dstf->ts = srcf->ts;
    }
    s->nb_frames -= n;
    return n;
}

static void request_seek(struct sfxmp_ctx *s, double t)
{
    if (s->can_seek_again) {
        DBG("main", "request seek at @ %f [cur:%f]\n", t, s->request_seek);
        s->request_seek = t;
    } else {
        DBG("main", "can not seek again, waiting\n");
    }
}

const struct sfxmp_frame *sfxmp_get_frame(struct sfxmp_ctx *s, double t)
{
    DBG("main", " >> get frame for t=%f\n", t);

    if (t < 0) {
        fprintf(stderr, "ERR: attempt to get a frame at a negative time\n");
        return NULL;
    }

    for (;;) {
        double diff;

        DBG("main", "loop check\n");
        pthread_mutex_lock(&s->queue_lock);
        DBG("main", "mutex acquired\n");

        if (s->queue_terminated && !s->nb_frames) {
            DBG("main", "spawn decoding thread\n");

            s->queue_terminated = 0;
            s->request_seek     = -1;
            s->can_seek_again   = 1;

            if (pthread_create(&s->dec_thread, NULL, decoder_thread, s)) {
                fprintf(stderr, "Unable to spawn decoding thread\n");
                s->queue_terminated = 1;
                pthread_mutex_unlock(&s->queue_lock);
                return NULL;
            }
        }

        if (t < s->visible_time) {
            DBG("main", "time requested before visible time, return nothing\n");
            pthread_mutex_unlock(&s->queue_lock);
            return ret_frame(s, &s->non_visible);
        }

        while (!s->nb_frames && !s->queue_terminated) {
            DBG("main", "queue is still empty, wait for it to grow\n");
            pthread_cond_wait(&s->queue_grow, &s->queue_lock);
        }

        if (!s->nb_frames && s->queue_terminated) {
            // this can happen if the decoding thread wasn't able to decode
            // anything
            pthread_mutex_unlock(&s->queue_lock);
            return NULL;
        }

        diff = get_frame_dist(s, 0, t);

        if (diff < 0) {
            /* past seek */
            request_seek(s, t);
            consume_queue(s, s->nb_frames);
            pthread_mutex_unlock(&s->queue_lock);
            pthread_cond_signal(&s->queue_reduce);
        } else {
            int best_id = 0;
            int i, need_more_frames;

            // skip N frames: this is useful when the refresh rate is lower
            // than the video frame rate (N frames to consume at after each
            // frame requested)
            for (i = 1; i < s->nb_frames; i++) {
                const double new_diff = get_frame_dist(s, i, t);
                if (new_diff > diff || new_diff < 0)
                    break;
                diff = new_diff;
                best_id = i;
            }
            need_more_frames = i == s->nb_frames && !s->queue_terminated && diff;
            DBG("main", "best frame: %d/%d | need more: %d (terminated:%d)\n",
                best_id+1, s->nb_frames, need_more_frames, s->queue_terminated);

            if (diff > s->dist_time_seek_trigger) /* future seek */
                request_seek(s, t);

            if (!consume_queue(s, best_id) && need_more_frames) {
                DBG("decoder", "nothing consumed but needs more frame, wait for grow\n");
                pthread_cond_wait(&s->queue_grow, &s->queue_lock);
                pthread_mutex_unlock(&s->queue_lock);
                continue;
            }

            pthread_mutex_unlock(&s->queue_lock);
            pthread_cond_signal(&s->queue_reduce);

            if (!need_more_frames) {
                DBG("main", "raise frame %p with ts=%f for t=%f\n",
                    s->frames[0].frame, s->frames[0].ts, t);
                return ret_frame(s, &s->frames[0]);
            } else {
                DBG("main", "need more frame for t=%f [%d frames in queue]\n", t, s->nb_frames);
            }
        }
    }
}
