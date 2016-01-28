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
#include <libavutil/log.h>
#include <libavutil/frame.h>
#include <libavutil/timestamp.h>

#include "sxplayer.h"
#include "async.h"

// TODO: some must be rutime configurable
// TODO: add logging user callback
#define ENABLE_INFO 0
#define ENABLE_DBG 0

void do_log(void *log_ctx, int log_level, const char *fn, const char *fmt, ...) av_printf_format(4, 5);

#define DO_LOG(c, log_level, ...) do_log((c)->log_ctx, log_level, __FUNCTION__, __VA_ARGS__)

#define INFO(c, ...)  DO_LOG(c, SXPLAYER_LOG_INFO, __VA_ARGS__)
#define LOG_ERROR(c, ...) DO_LOG(c, SXPLAYER_LOG_ERROR, __VA_ARGS__)
#define LOG(c, level, ...) DO_LOG(c, SXPLAYER_LOG_##level, __VA_ARGS__)

#if ENABLE_DBG
#define TRACE(c, ...) do { DO_LOG(c, SXPLAYER_LOG_VERBOSE, __VA_ARGS__); fflush(stdout); } while (0)
#else
/* Note: this could be replaced by a "while(0)" but it wouldn't test the
 * compilation of the printf format, so we use this more complex form. */
#define TRACE(c, ...) do { if (0) DO_LOG(c, SXPLAYER_LOG_VERBOSE, __VA_ARGS__); } while (0)
#endif

enum AVPixelFormat pix_fmts_sx2ff(enum sxplayer_pixel_format pix_fmt);
enum sxplayer_pixel_format pix_fmts_ff2sx(enum AVPixelFormat pix_fmt);
void set_thread_name(const char *name);

#define TIME2INT64(d) llrint((d) * av_q2d(av_inv_q(AV_TIME_BASE_Q)))
#define PTS2TIMESTR(t64) av_ts2timestr(t64, &AV_TIME_BASE_Q)

struct log_ctx {
    void *avlog;
    int64_t last_time;
    void *user_arg;
    void (*callback)(void *arg, int level, const char *fmt, va_list vl);
};

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
};

#endif
