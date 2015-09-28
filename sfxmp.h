#ifndef SFXMP_H
#define SFXMP_H

#include <stdint.h>

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

struct sfxmp_ctx;

enum sfxmp_media_selection {
    SFXMP_SELECT_VIDEO,
    SFXMP_SELECT_AUDIO,
    NB_SFXMP_MEDIA_SELECTION // *NOT* part of the API/ABI
};

enum sfxmp_pixel_format {
    SFXMP_PIXFMT_RGBA,
    SFXMP_PIXFMT_BGRA,
    SFXMP_PIXFMT_VT,
};

struct sfxmp_frame {
    uint8_t *data;      // native RGBA/ABGR plane
    double ts;          // video timestamp
    int linesize;       // linesize in bytes (includes padding)
    int width;          // frame width in pixel
    int height;         // frame height in pixel
    int pix_fmt;        // sfxmp_pixel_format
    void *internal;     // sfxmp internal frame context frame, do not alter
};

/**
 * Create media player context
 *
 * @param filename media input file name
 */
struct sfxmp_ctx *sfxmp_create(const char *filename);

/**
 * Set an option.
 *
 * Available options:
 *
 *   key                      type      description
 *   ----------------------------------------------
 *   avselect                 int       select audio or video stream (see SFXMP_SELECT_*)
 *   skip                     double    time to skip in the specified input
 *   trim_duration            double    duration of the video (starting at skip)
 *   dist_time_seek_trigger   double    how much time forward will trigger a seek, can be negative for default
 *   max_nb_frames            integer   maximum number of frames in the queue, can be negative for default
 *   filters                  string    custom user filters (software decoding only)
 *   sw_pix_fmt               integer   pixel format format to use when using software decoding (video only)
 *   autorotate               integer   automatically insert rotation filters (video software decoding only)
 *   auto_hwaccel             integer   attempt to enable hardware acceleration
 */
int sfxmp_set_option(struct sfxmp_ctx *s, const char *key, ...);

/* Get the frame at an absolute time. The returned frame can be NULL if
 * unchanged from last call. The returned frame needs to be released using
 * sfxmp_release_frame(). */
struct sfxmp_frame *sfxmp_get_frame(struct sfxmp_ctx *s, double t);

/* Enable or disable the droping of non reference frames */
int sfxmp_set_drop_ref(struct sfxmp_ctx *s, int drop);

/* Release a frame obtained with sfxmp_get_frame() */
void sfxmp_release_frame(struct sfxmp_frame *frame);

/* Close and free everything */
void sfxmp_free(struct sfxmp_ctx **ss);

#endif
