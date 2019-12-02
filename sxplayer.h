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

#define SXPLAYER_VERSION_MAJOR 9
#define SXPLAYER_VERSION_MINOR 5
#define SXPLAYER_VERSION_MICRO 1

#define SXPLAYER_GET_VERSION(major, minor, micro) ((major)<<16 | (minor)<<8 | (micro))

#define SXPLAYER_VERSION_INT SXPLAYER_GET_VERSION(SXPLAYER_VERSION_MAJOR, \
                                                  SXPLAYER_VERSION_MINOR, \
                                                  SXPLAYER_VERSION_MICRO)

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
    SXPLAYER_PIXFMT_MEDIACODEC,// MediaCodec pixel format (HW accelerated, frame->data is a AVMediaCodecBuffer)
    SXPLAYER_SMPFMT_FLT,
    SXPLAYER_PIXFMT_VAAPI,     // VAAPI pixel format (HW accelerated, frame->data is a VASurfaceID)
};

enum sxplayer_loglevel {
    SXPLAYER_LOG_VERBOSE,
    SXPLAYER_LOG_DEBUG,
    SXPLAYER_LOG_INFO,
    SXPLAYER_LOG_WARNING,
    SXPLAYER_LOG_ERROR,
};

enum {
    SXPLAYER_COL_SPC_RGB,
    SXPLAYER_COL_SPC_BT709,
    SXPLAYER_COL_SPC_UNSPECIFIED,
    SXPLAYER_COL_SPC_RESERVED,
    SXPLAYER_COL_SPC_FCC,
    SXPLAYER_COL_SPC_BT470BG,
    SXPLAYER_COL_SPC_SMPTE170M,
    SXPLAYER_COL_SPC_SMPTE240M,
    SXPLAYER_COL_SPC_YCGCO,
    SXPLAYER_COL_SPC_BT2020_NCL,
    SXPLAYER_COL_SPC_BT2020_CL,
    SXPLAYER_COL_SPC_SMPTE2085,
    SXPLAYER_COL_SPC_CHROMA_DERIVED_NCL,
    SXPLAYER_COL_SPC_CHROMA_DERIVED_CL,
    SXPLAYER_COL_SPC_ICTCP,
    NB_SXPLAYER_COL_SPC // *NOT* part of API/ABI
};

enum {
    SXPLAYER_COL_RNG_UNSPECIFIED,
    SXPLAYER_COL_RNG_LIMITED,
    SXPLAYER_COL_RNG_FULL,
    NB_SXPLAYER_COL_RNG // *NOT* part of API/ABI
};

enum {
    SXPLAYER_COL_PRI_RESERVED0,
    SXPLAYER_COL_PRI_BT709,
    SXPLAYER_COL_PRI_UNSPECIFIED,
    SXPLAYER_COL_PRI_RESERVED,
    SXPLAYER_COL_PRI_BT470M,
    SXPLAYER_COL_PRI_BT470BG,
    SXPLAYER_COL_PRI_SMPTE170M,
    SXPLAYER_COL_PRI_SMPTE240M,
    SXPLAYER_COL_PRI_FILM,
    SXPLAYER_COL_PRI_BT2020,
    SXPLAYER_COL_PRI_SMPTE428,
    SXPLAYER_COL_PRI_SMPTE431,
    SXPLAYER_COL_PRI_SMPTE432,
    SXPLAYER_COL_PRI_JEDEC_P22,
    NB_SXPLAYER_COL_PRI // *NOT* part of API/ABI
};

enum {
    SXPLAYER_COL_TRC_RESERVED0,
    SXPLAYER_COL_TRC_BT709,
    SXPLAYER_COL_TRC_UNSPECIFIED,
    SXPLAYER_COL_TRC_RESERVED,
    SXPLAYER_COL_TRC_GAMMA22,
    SXPLAYER_COL_TRC_GAMMA28,
    SXPLAYER_COL_TRC_SMPTE170M,
    SXPLAYER_COL_TRC_SMPTE240M,
    SXPLAYER_COL_TRC_LINEAR,
    SXPLAYER_COL_TRC_LOG,
    SXPLAYER_COL_TRC_LOG_SQRT,
    SXPLAYER_COL_TRC_IEC61966_2_4,
    SXPLAYER_COL_TRC_BT1361_ECG,
    SXPLAYER_COL_TRC_IEC61966_2_1,
    SXPLAYER_COL_TRC_BT2020_10,
    SXPLAYER_COL_TRC_BT2020_12,
    SXPLAYER_COL_TRC_SMPTE2084,
    SXPLAYER_COL_TRC_SMPTE428,
    SXPLAYER_COL_TRC_ARIB_STD_B67,
    NB_SXPLAYER_COL_TRC // *NOT* part of the API/ABI
};

struct sxplayer_frame {
    uint8_t *data;      // frame data in RGBA, BGRA, ... according to pix_fmt
    double ts;          // video timestamp
    int linesize;       // linesize in bytes (includes padding)
    union {
    int width;          // frame width in pixel
    int nb_samples;     // number of audio samples contained in the frame
    };
    int height;         // frame height in pixel
    int pix_fmt;        // sxplayer_pixel_format
    void *mvs;          // motions vectors (AVMotionVector*)
    int nb_mvs;         // number of motions vectors
    int64_t ms;         // video timestamp in microseconds
    int64_t pts;        // video presentation time stamp in stream timebase unit
    void *internal;     // sxplayer internal frame context frame, do not alter
    int color_space;    // video color space (any of SXPLAYER_COL_CSP_*)
    int color_range;    // video color range (any of SXPLAYER_COL_RNG_*)
    int color_primaries;// video color primaries (any of SXPLAYER_COL_PRI_*)
    int color_trc;      // video color transfer (any of SXPLAYER_COL_TRC_*)
};

struct sxplayer_info {
    int width;
    int height;
    double duration;
    int is_image;
    int timebase[2];    // stream timebase
};

/**
 * Create media player context
 *
 * @param filename media input file name
 */
struct sxplayer_ctx *sxplayer_create(const char *filename);

/**
 * Type of the user log callback
 *
 * @param arg       opaque user argument
 * @param level     log level of the message (SXPLAYER_LOG_*)
 * @param filename  source filename
 * @param ln        line number in the source file
 * @param fn        function name in the source
 * @param fmt       log string format
 * @param vl        variable argument list associated with the format
 */
typedef void (*sxplayer_log_callback_type)(void *arg, int level, const char *filename,
                                           int ln, const char *fn, const char *fmt, va_list vl);

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
void sxplayer_set_log_callback(struct sxplayer_ctx *s, void *arg, sxplayer_log_callback_type callback);

/**
 * Set an option.
 *
 * Available options:
 *
 *   key                      type      description
 *   ----------------------------------------------
 *   avselect                 integer   select audio or video stream (see SXPLAYER_SELECT_*)
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
 *   opaque                   binary    pointer to an opaque pointer forwarded to the decoder (for example, a pointer to an android/view/Surface to use in conjonction with the mediacodec decoder)
 *   max_pixels               integer   maximum number of pixels per frame
 *   audio_texture            integer   output audio as a video texture
 *   vt_pix_fmt               string    VideoToolbox pixel format in the CVPixelBufferRef ("bgra" or "nv12")
 *   stream_idx               integer   force a stream number instead of picking the "best" one (note: stream MUST be of type avselect)
 *   use_pkt_duration         integer   use packet duration instead of decoding the next frame to get the next frame pts
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
 *
 * The function is blocking, it will make sure any asynchronous operation
 * previously requested (start, seek, stop) is honored before returning.
 */
struct sxplayer_frame *sxplayer_get_frame(struct sxplayer_ctx *s, double t);

/**
 * Same as sxplayer_get_frame, but with timestamp expressed in microseconds.
 */
struct sxplayer_frame *sxplayer_get_frame_ms(struct sxplayer_ctx *s, int64_t ms);

/**
 * Request a playback start to the player.
 *
 * The function always returns immediately (it doesn't wait for a frame to be
 * decoded).
 *
 * Return 0 on success, a negative value on error.
 */
int sxplayer_start(struct sxplayer_ctx *s);

/**
 * Request a stop to the player to liberate playback ressources.
 *
 * The function always returns immediately (it doesn't wait for every
 * ressources and contexts to be destroyed).
 *
 * Return 0 on success, a negative value on error.
 */
int sxplayer_stop(struct sxplayer_ctx *s);

/**
 * Request a seek to the player at a given time.
 *
 * The function always returns immediately (the seek will be delayed and
 * executed in another thread).
 *
 * Note: the passed time is relative to the skip option.
 *
 * Return 0 on success, a negative value on error.
 */
int sxplayer_seek(struct sxplayer_ctx *s, double t);

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
