#ifndef SFXMP_H
#define SFXMP_H

#include <stdint.h>

/* Stupeflix Media Player */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 *                      +------+------------------------------+---------+
 *  Video timeline      |     #|##############################|         |
 *                      +------+------------------------------+---------+
 *                      ^     ^^                              ^
 *                    Vt=0   Vt=skip               Vt=skip+trim_duration
 *                          Vt=first_pts
 *                             <----------------------------->
 *                                      trim_duration
 *
 *  Vt: video timestamp
 *      ...
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct sfxmp_ctx;

enum sfxmp_media_selection {
    SFXMP_SELECT_VIDEO,
    SFXMP_SELECT_AUDIO,
};

enum sfxmp_pixel_format {
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
 * @param avselect               select audio or video stream (see SFXMP_SELECT_*)
 * @param skip                   time to skip in the specified input
 * @param trim_duration          duration of the video (starting at skip)
 * @param dist_time_seek_trigger how much time forward will trigger a seek, can be negative for default
 * @param max_nb_frames          maximum number of frames in the queue, can be negative for default
 * @param filters                custom user filters, can be NULL
 */
struct sfxmp_ctx *sfxmp_create(const char *filename,
                               int avselect,
                               double skip,
                               double trim_duration,
                               double dist_time_seek_trigger,
                               double max_nb_frames,
                               const char *filters);

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
