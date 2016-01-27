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

#ifndef SXPLAYER_H
#define SXPLAYER_H

#include <stdint.h>
#include <stdarg.h>

/* Stupeflix Media Player */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 *                                            trim_duration
 *                                  <------------------------------>
 *
 *                                 t=0                            t=END
 *                                  v                              v
 *                       -----------+------------------------------+---------
 *      timeline           PREFECH  |##############################| LAST
 *                       -----------+------------------------------+---------
 *                                 .                              .
 *                                .                              .
 *                               .                              .
 *                              .                              .
 *                      +------+------------------------------+---------+
 *  Video timeline      |     #|##############################|         |
 *                      +------+------------------------------+---------+
 *                      ^     ^^                              ^
 *                    Vt=0   Vt=skip               Vt=skip+trim_duration
 *                             <------------------------------>
 *                                      trim_duration
 *
 *  t:  timeline timestamp
 *  Vt: video timestamp
 *  PREFETCH: starts thread if necessary and returns NULL
 *  LAST: repeat last frame (or like any other frame NULL if it was already raised)
 *      ...
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct sxplayer_ctx;

enum sxplayer_media_selection {
    SXPLAYER_SELECT_VIDEO,
    SXPLAYER_SELECT_AUDIO,
    NB_SXPLAYER_MEDIA_SELECTION // *NOT* part of the API/ABI
};

enum sxplayer_pixel_format {
    SXPLAYER_PIXFMT_RGBA,
    SXPLAYER_PIXFMT_BGRA,
    SXPLAYER_PIXFMT_VT,        // VideoToolBox pixel format (HW accelerated, frame->data is a CVPixelBufferRef)
};

enum sxplayer_loglevel {
    SXPLAYER_LOG_VERBOSE,
    SXPLAYER_LOG_DEBUG,
    SXPLAYER_LOG_INFO,
    SXPLAYER_LOG_WARNING,
    SXPLAYER_LOG_ERROR,
};

struct sxplayer_frame {
    uint8_t *data;      // frame data in RGBA, BGRA, ... according to pix_fmt
    double ts;          // video timestamp
    int linesize;       // linesize in bytes (includes padding)
    int width;          // frame width in pixel
    int height;         // frame height in pixel
    int pix_fmt;        // sxplayer_pixel_format
    void *mvs;          // motions vectors (AVMotionVector*)
    int nb_mvs;         // number of motions vectors
    void *internal;     // sxplayer internal frame context frame, do not alter
};

struct sxplayer_info {
    int width;
    int height;
    double duration;
};

/**
 * Create media player context
 *
 * @param filename media input file name
 */
struct sxplayer_ctx *sxplayer_create(const char *filename);

/**
 * Set user logging callback
 *
 * Setting the logging callback disables the local logging, and every log
 * messages at every level are forwarded to the user through the specified
 * callback.
 *
 * @param arg       opaque user argument to be sent back as first argument in
 *                  the callback
 * @param callback  custom user logging callback
 */
void sxplayer_set_log_callback(struct sxplayer_ctx *s, void *arg,
                               void (*callback)(void *arg, int level, const char *fmt, va_list vl));

/**
 * Set an option.
 *
 * Available options:
 *
 *   key                      type      description
 *   ----------------------------------------------
 *   avselect                 int       select audio or video stream (see SXPLAYER_SELECT_*)
 *   skip                     double    time to skip in the specified input
 *   trim_duration            double    duration of the video (starting at skip)
 *   dist_time_seek_trigger   double    how much time forward will trigger a seek
 *   max_nb_frames            integer   maximum number of frames in the queue
 *   filters                  string    custom user filters (software decoding only)
 *   sw_pix_fmt               integer   pixel format format to use when using software decoding (video only), can be any SXPLAYER_PIXFMT_* not HW accelerated
 *   autorotate               integer   automatically insert rotation filters (video software decoding only)
 *   auto_hwaccel             integer   attempt to enable hardware acceleration
 *   export_mvs               integer   export motion vectors into frame->mvs
 *   pkt_skip_mod             integer   skip packet if module pkt_skip_mod (and not a key pkt)
 */
int sxplayer_set_option(struct sxplayer_ctx *s, const char *key, ...);

/**
 * Get the media duration (clipped to trim_duration if set).
 *
 * The duration is expressed in seconds.
 */
int sxplayer_get_duration(struct sxplayer_ctx *s, double *duration);

/**
 * Get various information on the media.
 */
int sxplayer_get_info(struct sxplayer_ctx *s, struct sxplayer_info *info);

/**
 * Get the frame at an absolute time.
 *
 * The returned frame can be NULL if unchanged from last call.
 *
 * The returned frame needs to be released using sxplayer_release_frame().
 *
 * Requesting a negative time is equivalent to calling sxplayer_prefetch().
 *
 * If you are working on a player (which typically needs seeking and has a
 * refresh rate architecture), this is the function you are probably interested
 * in.
 */
struct sxplayer_frame *sxplayer_get_frame(struct sxplayer_ctx *s, double t);

/**
 * Start the decoding threads and return.
 *
 * The function always returns immediately (it doesn't wait for a frame to be
 * decoded).
 *
 * Return 0 on success, a negative value on error.
 */
int sxplayer_prefetch(struct sxplayer_ctx *s);

/**
 * Get the next frame.
 *
 * The returned frame needs to be released using sxplayer_release_frame().
 *
 * At EOF sxplayer_get_next_frame() will return NULL. You can call
 * sxplayer_get_next_frame() to restart the decoding from the beginning.
 *
 * If you want to process every single frame of the media regardless of a
 * "refresh rate" or seeking needs, this is the function you are probably
 * interested in. You can still use this function in combination with
 * sxplayer_get_frame() in case you need seeking.
 */
struct sxplayer_frame *sxplayer_get_next_frame(struct sxplayer_ctx *s);

/* Enable or disable the droping of non reference frames */
int sxplayer_set_drop_ref(struct sxplayer_ctx *s, int drop);

/* Release a frame obtained with sxplayer_get_frame() */
void sxplayer_release_frame(struct sxplayer_frame *frame);

/* Close and free everything */
void sxplayer_free(struct sxplayer_ctx **ss);

#endif
