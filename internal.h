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

#ifndef SXPLAYER_INTERNAL_H
#define SXPLAYER_INTERNAL_H

#include <stdio.h>
#include <libavutil/frame.h>
#include <libavutil/timestamp.h>
#include <libavcodec/version.h>

#include "sxplayer.h"
#include "log.h"
#include "async.h"

#define HAVE_MEDIACODEC_HWACCEL LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 49, 103)

enum AVPixelFormat pix_fmts_sx2ff(enum sxplayer_pixel_format pix_fmt);
enum sxplayer_pixel_format pix_fmts_ff2sx(enum AVPixelFormat pix_fmt);
enum sxplayer_pixel_format smp_fmts_ff2sx(enum AVSampleFormat smp_fmt);
void set_thread_name(const char *name);
void update_dimensions(int *width, int *height, int max_pixels);

#define TIME2INT64(d) llrint((d) * av_q2d(av_inv_q(AV_TIME_BASE_Q)))
#define PTS2TIMESTR(t64) av_ts2timestr(t64, &AV_TIME_BASE_Q)

struct sxplayer_ctx {
    const AVClass *class;                   // necessary for the AVOption mechanism
    struct log_ctx *log_ctx;
    char *filename;                         // input filename
    char *logname;

    /* configurable options */
    int avselect;                           // select audio or video
    double skip;                            // see public header
    double trim_duration;                   // see public header
    double dist_time_seek_trigger;          // see public header
    int max_nb_frames;                      // maximum number of frames in the queue
    int max_nb_packets;                     // maximum number of packets in the queue
    int max_nb_sink;                        // maximum number of frames in the filtered queue
    char *filters;                          // user filter graph string
    int sw_pix_fmt;                         // sx pixel format to use for software decoding
    int autorotate;                         // switch for automatically rotate in software decoding
    int auto_hwaccel;                       // attempt to enable hardware acceleration
    int export_mvs;                         // export motion vectors into frame->mvs
    int pkt_skip_mod;                       // skip packet if module pkt_skip_mod (and not a key pkt)
    int thread_stack_size;
    void *opaque;                           // pointer to an opaque pointer forwarded to the decoder
    int opaque_size;                        // opaque pointer size
    int max_pixels;                         // maximum number of pixels per frame
    int audio_texture;                      // output audio as a video texture
    char *vt_pix_fmt;                       // VideoToolbox pixel format in the CVPixelBufferRef

    struct async_context *actx;
    int64_t skip64;
    int64_t trim_duration64;
    int64_t dist_time_seek_trigger64;
    int context_configured;

    AVFrame *cached_frame;
    int64_t last_pushed_frame_ts;           // ts value of the latest pushed frame (it acts as a UID)
    int64_t last_frame_poped_ts;
    int64_t first_ts;
    int64_t last_ts;
    int64_t entering_time;
    int last_op_was_prefetch;
};

#endif
