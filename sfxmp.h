#ifndef SFXMP_H
#define SFXMP_H

#include <stdint.h>

/* Stupeflix Media Player */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 *                                                        trim_duration
 *                                                      <--------------->
 *
 *                     t=0    t=visible_time      t=start_time        t=END
 *                      v           v                   v               v
 *                      +-----------+-------------------+---------------+---------
 *      timeline        | B B B B B | 0 0 0 0 0 0 0 0 0 | 1 2 3 4 5 6 7 | 7 7 7 7
 *                      +-----------+-------------------+---------------+---------
 *                                 .                                 ´
 *                                .                                ´
 *                               .                               ´
 *                              .                              ´
 *                      +------+------------------------------+---------+
 *  Video timeline      |     #|##############################|         |
 *                      +------+------------------------------+---------+
 *                      ^     ^^                              ^
 *                    Vt=0   Vt=skip               Vt=skip+trim_duration
 *                          Vt=first_pts
 *                             <----------------------------->
 *                                      trim_duration
 *
 *  t:  timeline timestamp
 *  Vt: video timestamp
 *  B:  black frame
 *  0:  frame 0
 *  1:  frame 1
 *  2:  frame 2
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
 * @param visible_time           everything before this time will be black
 * @param start_time             between visible_time and start_time, first frame will be displayed
 * @param skip                   time to skip in the specified input
 * @param trim_duration          duration of the video (starting at skip for video time, or start_time for timeline)
 * @param dist_time_seek_trigger how much time forward will trigger a seek, can be negative for default
 * @param max_nb_frames          maximum number of frames in the queue, can be negative for default
 * @param filters                custom user filters, can be NULL
 */
struct sfxmp_ctx *sfxmp_create(const char *filename,
                               int avselect,
                               double visible_time,
                               double start_time,
                               double skip,
                               double trim_duration,
                               double dist_time_seek_trigger,
                               double max_nb_frames,
                               const char *filters);

/* Get the frame at an absolute time. The returned frame can be NULL if
 * unchanged from last call. The returned frame needs to be released using
 * sfxmp_release_frame(). */
struct sfxmp_frame *sfxmp_get_frame(struct sfxmp_ctx *s, double t);

/* Release a frame obtained with sfxmp_get_frame() */
void sfxmp_release_frame(struct sfxmp_frame *frame);

/* Close and free everything */
void sfxmp_free(struct sfxmp_ctx **ss);

#endif
