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
 *  OSG timeline        | B B B B B | 0 0 0 0 0 0 0 0 0 | 1 2 3 4 5 6 7 | 7 7 7 7
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
 *  t:  OSG timestamp
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

struct sfxmp_frame {
    uint8_t *data;
    int linesize;
    int width;
    int height;
};

/* Create media player context */
struct sfxmp_ctx *sfxmp_create(const char *filename,
                               int avselect,
                               double visible_time,
                               double start_time,
                               double skip,
                               double trim_duration);

/* Get the frame at an absolute time. The returned frame can be NULL if
 * unchanged from last call. It stays readable until the next call to this
 * function (or call to sfxmp_free()). */
const struct sfxmp_frame *sfxmp_get_frame(struct sfxmp_ctx *s, double t);

/* Close and free everything */
void sfxmp_free(struct sfxmp_ctx **ss);

#endif
