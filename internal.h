/*
 * This file is part of sfxmp.
 *
 * Copyright (c) 2015 Stupeflix
 *
 * sfxmp is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * sfxmp is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with sfxmp; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef SFXMP_INTERNAL_H
#define SFXMP_INTERNAL_H

#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/avfft.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>

#include "decoders.h"
#include "async.h"

#define ENABLE_DBG 0

void do_log(const char *mod, const char *fmt, ...);

#define DBG_SFXMP(mod, ...) do { do_log(mod, __VA_ARGS__); fflush(stdout); } while (0)
#if ENABLE_DBG
# define DBG(mod, ...) DBG_SFXMP(mod, __VA_ARGS__)
#else
/* Note: this could be replaced by a "while(0)" but it wouldn't test the
 * compilation of the printf format, so we use this more complex form. */
# define DBG(mod, ...) do { if (0) DBG_SFXMP(mod, __VA_ARGS__); } while (0)
#endif

#define TIME2INT64(d) llrint((d) * av_q2d(av_inv_q(AV_TIME_BASE_Q)))
#define PTS2TIMESTR(t64) av_ts2timestr(t64, &AV_TIME_BASE_Q)

#define AUDIO_NBITS      10
#define AUDIO_NBSAMPLES  (1<<(AUDIO_NBITS))
#define AUDIO_NBCHANNELS 2

struct sfxmp_ctx {
    const AVClass *class;                   // necessary for the AVOption mechanism
    char *filename;                         // input filename

    int context_configured;                 // set if options are pre-processed, file is opened, ...

    /* configurable options */
    int avselect;                           // select audio or video
    double skip;                            // see public header
    double trim_duration;                   // see public header
    double dist_time_seek_trigger;          // distance time triggering a seek
    int max_nb_frames;                      // maximum number of frames in the queue
    int max_nb_packets;                     // maximum number of packets in the queue
    char *filters;                          // user filter graph string
    int sw_pix_fmt;                         // sx pixel format to use for software decoding
    int autorotate;                         // switch for automatically rotate in software decoding
    int auto_hwaccel;                       // attempt to enable hardware acceleration
    int export_mvs;                         // export motion vectors into frame->mvs
    int pkt_skip_mod;                       // skip packet if module pkt_skip_mod (and not a key pkt)

    int64_t skip64;
    int64_t trim_duration64;
    int64_t dist_time_seek_trigger64;

    /* misc general fields */
    enum AVMediaType media_type;            // AVMEDIA_TYPE_{VIDEO,AUDIO} according to avselect
    const char *media_type_string;          // "audio" or "video" according to avselect

    /* ... */
    struct async_context *actx;
    struct async_reader *reader;
    struct async_decoder *adec;
    AVFrame *cached_frame;
    AVFrame *queued_frame;
    int64_t pkt_count;

    /* main vs demuxer/decoder thread negotiation */
    pthread_t dec_thread;                   // decoding thread
    pthread_mutex_t lock;
    pthread_cond_t cond;
#define THREAD_STATE_NOTRUNNING 0
#define THREAD_STATE_RUNNING    1
#define THREAD_STATE_DYING      2 // decoding thread is dying, and need to be joined before being restarted
    int thread_state;

    /* fields specific to main thread */
    int can_seek_again;                     // field used to avoid seeking again until the requested time is reached
    int request_drop;                       // field used by the main thread to request a change in the frame dropping mechanism
    int64_t last_pushed_frame_ts;           // ts value of the latest pushed frame (it acts as a UID)
    int64_t first_ts;

    /* fields specific to decoding thread */
    AVFrame *filtered_frame;                // filtered version of decoded_frame
    AVFrame *audio_texture_frame;           // wave/fft texture in case of audio
    AVFrame *tmp_audio_frame;
    AVFormatContext *fmt_ctx;               // demuxing context
    struct decoder_ctx *dec_ctx;            // decoder context
    AVCodec *dec;
    AVStream *stream;                       // selected stream
    int stream_idx;                         // selected stream index
    AVFilterGraph *filter_graph;            // libavfilter graph
    AVFilterContext *buffersink_ctx;        // sink of the graph (from where we pull)
    AVFilterContext *buffersrc_ctx;         // source of the graph (where we push)
    float *window_func_lut;                 // audio window function lookup table
    RDFTContext *rdft;                      // real discrete fourier transform context
    FFTSample *rdft_data[AUDIO_NBCHANNELS]; // real discrete fourier transform data for each channel
    enum AVPixelFormat last_frame_format;   // format of the last frame decoded
};

#endif
