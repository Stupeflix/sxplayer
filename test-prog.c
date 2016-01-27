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

#include <libavutil/avassert.h>
#include <libavutil/common.h>
#include <libavutil/time.h>
#include <float.h> // for DBL_MAX

#include "sxplayer.h"

#define N 4
#define NB_FRAMES (1<<(3*N))
#define SOURCE_FPS 25

#define PRE_FILL_TIME 3     /* try to pre fetch starting 3 seconds earlier */
#define POST_REQUEST_TIME 2 /* attempt to fetch the last frame for 2 more seconds */

static int test_frame(struct sxplayer_ctx *s,
                      double time, int *prev_frame_id, double *prev_time,
                      double skip, double trim_duration, int avselect)
{
    struct sxplayer_frame *frame = sxplayer_get_frame(s, time);

    if (time < 0) {
        if (frame) {
            fprintf(stderr, "Time requested < 0 but got frame\n");
            sxplayer_release_frame(frame);
            return -1;
        }
        return 0;
    }

    if (avselect == SXPLAYER_SELECT_AUDIO) {
        // TODO
        if (frame) {
            const double playback_time = av_clipd(time, 0, trim_duration < 0 ? DBL_MAX : trim_duration);
            const double diff = FFABS(playback_time - frame->ts);
            printf("AUDIO TEST FRAME %dx%d pt:%f ft:%f diff:%f\n", frame->width, frame->height, playback_time, frame->ts, diff);
        }
        sxplayer_release_frame(frame);
        return 0;
    }

    if (frame) {
        const uint32_t c = *(const uint32_t *)frame->data;
        const int r = c >> (N+16) & 0xf;
        const int g = c >> (N+ 8) & 0xf;
        const int b = c >> (N+ 0) & 0xf;

        const int frame_id = r<<(N*2) | g<<N | b;
        const double video_ts = frame_id * 1. / SOURCE_FPS;
        const double estimated_time_from_color = video_ts - skip;
        const double playback_time = av_clipd(time, 0, trim_duration < 0 ? DBL_MAX : trim_duration);
        const double diff = FFABS(playback_time - estimated_time_from_color);

        printf("frame t=%f (pt=%f) -> %p color %08X => video_ts:%f, frame expected at t=%f [diff:%f]\n",
               time, playback_time, frame, c, video_ts, estimated_time_from_color, diff);

        if (diff > 2./SOURCE_FPS) {
            fprintf(stderr, "ERROR: frame diff is too big %f>%f\n", diff, 2./SOURCE_FPS);
            sxplayer_release_frame(frame);
            return -1;
        }

        if (frame_id == *prev_frame_id) {
            fprintf(stderr, "ERROR: Got a frame, but it has the same content as the previous one\n");
            sxplayer_release_frame(frame);
            return -1;
        }

        *prev_frame_id = frame_id;
    } else {

        if (time < *prev_time) {
            fprintf(stderr, "ERROR: time went backward but got no frame update\n");
            sxplayer_release_frame(frame);
            return -1;
        }

        if (time - *prev_time > 1.5/SOURCE_FPS) {
            if (time > 0 && time < trim_duration) {
                fprintf(stderr, "ERROR: the difference between current and previous time (%f) "
                        "is large enough to get a new frame, but got none\n", time - *prev_time);
                sxplayer_release_frame(frame);
                return -1;
            }
        }

        printf("frame t=%f: no frame\n", time);
    }
    *prev_time = time;
    sxplayer_release_frame(frame);
    return 0;
}

static int test_instant_gets(const char *filename, int avselect)
{
    int i, ret = 0;
    const double skip          = 27.2;
    const double trim_duration = 67.1;

    const double instant_gets[] = {-1, 0.5, 17.2, 26.2, 38.4, 89.7, 97.6, 102.4};

    printf("Test: %s\n", __FUNCTION__);

    for (i = 0; i < FF_ARRAY_ELEMS(instant_gets); i++) {
        int prev_frame_id = -1;
        double prev_time = -DBL_MAX;

        struct sxplayer_ctx *s = sxplayer_create(filename);

        printf("Test instant get @ t=%f\n", instant_gets[i]);

        sxplayer_set_option(s, "avselect", avselect);
        sxplayer_set_option(s, "skip", skip);
        sxplayer_set_option(s, "trim_duration", trim_duration);
        sxplayer_set_option(s, "auto_hwaccel", 0);

        if (!s)
            return -1;

        ret = test_frame(s, instant_gets[i], &prev_frame_id, &prev_time,
                         skip, trim_duration, avselect);

        sxplayer_free(&s);

        if (ret < 0)
            break;
    }
    return ret;
}

static int test_seeks(const char *filename, int avselect)
{
    int i, ret = 0;
    const double skip          = 45.;
    const double trim_duration = 120.;

    const double instant_gets[] = {32., 31., 31.2, 60.};

    struct sxplayer_ctx *s = sxplayer_create(filename);

    printf("Test: %s\n", __FUNCTION__);

    sxplayer_set_option(s, "avselect", avselect);
    sxplayer_set_option(s, "skip", skip);
    sxplayer_set_option(s, "trim_duration", trim_duration);
    sxplayer_set_option(s, "auto_hwaccel", 0);

    if (!s)
        return -1;

    for (i = 0; i < FF_ARRAY_ELEMS(instant_gets); i++) {
        int prev_frame_id = -1;
        double prev_time = -DBL_MAX;

        ret = test_frame(s, instant_gets[i], &prev_frame_id, &prev_time,
                         skip, trim_duration, avselect);

        if (ret < 0)
            break;
    }

    sxplayer_free(&s);
    return ret;
}

static int test_full_run(const char *filename, int refresh_rate,
                         double skip, double trim_duration,
                         int avselect)
{
    int i, ret = 0, prev_frame_id = -1;
    double prev_time = -DBL_MAX;
    const double request_end_time   = trim_duration + POST_REQUEST_TIME;
    const double request_duration   = request_end_time;
    const int nb_calls = request_duration * refresh_rate;

    struct sxplayer_ctx *s = sxplayer_create(filename);

    printf("Test: %s\n", __FUNCTION__);

    sxplayer_set_option(s, "avselect", avselect);
    sxplayer_set_option(s, "skip", skip);
    sxplayer_set_option(s, "trim_duration", trim_duration);
    sxplayer_set_option(s, "auto_hwaccel", 0);

    if (!s)
        return -1;

    if (!filename)
        goto end;

    printf("test full run of %s [%dFPS]\n",
           filename, SOURCE_FPS /* FIXME: probe from file */);

    printf("    skip:%f trim_duration:%f\n", skip, trim_duration);

    printf("    request: %f @ %dHz => nb_calls:%d\n",
           request_end_time, refresh_rate, nb_calls);

    for (i = 0; i < nb_calls; i++) {
        const double time = i / (double)refresh_rate;

        printf("TEST %d/%d with time=%f\n", i + 1, nb_calls, time);

        ret = test_frame(s, time, &prev_frame_id, &prev_time,
                         skip, trim_duration,
                         avselect);
        if (ret < 0)
            break;
    }

end:
    sxplayer_free(&s);
    return ret;
}

static int run_tests(const char *filename, int avselect)
{
    if (test_seeks(filename, avselect) < 0)
        return -1;

    if (test_full_run("dummy", 0, 0, 0, avselect) < 0 ||
        test_full_run(filename, 30, 0, -1, avselect) < 0 ||
        test_full_run(filename, 10, 1.1, 18.6, avselect) < 0 ||
        test_full_run(filename, 10, 1.1, 18.6, avselect) < 0 ||
        test_full_run(filename, 60, 3.7, 12.2, avselect) < 0)
        return -1;

    if (test_instant_gets(filename, avselect) < 0)
        return -1;

    return 0;
}

static int simple_pass_through(const char *filename)
{
    int i = 0, ret = 0;
    struct sxplayer_ctx *s = sxplayer_create(filename);

    sxplayer_set_option(s, "auto_hwaccel", 0);

    for (;;) {
        const double t = av_gettime();
        struct sxplayer_frame *frame = sxplayer_get_next_frame(s);
        const double diff = (av_gettime() - t) / 1000000.;

        if (!frame) {
            printf("null frame\n");
            break;
        }
        printf("[%f] frame #%d / data:%p ts:%f %dx%d lz:%d sfxpixfmt:%d\n",
               diff, i++, frame->data, frame->ts, frame->width, frame->height,
               frame->linesize, frame->pix_fmt);

        sxplayer_release_frame(frame);

        /* test code to make sure a NULL is returned even when a decoding
         * thread is restarted */
#if 0
        if (i % 4096 == 0) {
            usleep(1000000);
            av_assert0(!sxplayer_get_next_frame(s));
        }
#endif
    }

    sxplayer_free(&s);
    return ret;
}

static int test_next_frame(const char *filename)
{
    int i = 0, ret = 0, r;
    struct sxplayer_ctx *s = sxplayer_create(filename);

    sxplayer_set_option(s, "auto_hwaccel", 0);

    for (r = 0; r < 2; r++) {
        printf("Test: %s run #%d\n", __FUNCTION__, r+1);
        for (;;) {
            struct sxplayer_frame *frame = sxplayer_get_next_frame(s);

            if (!frame) {
                printf("null frame\n");
                break;
            }
            printf("frame #%d / data:%p ts:%f %dx%d lz:%d sfxpixfmt:%d\n",
                   i++, frame->data, frame->ts, frame->width, frame->height,
                   frame->linesize, frame->pix_fmt);

            sxplayer_release_frame(frame);
        }
    }

    sxplayer_free(&s);
    return ret;
}

static int test_duration(const char *filename)
{
    int ret;
    double duration;
    struct sxplayer_ctx *s = sxplayer_create(filename);

    ret = sxplayer_get_duration(s, &duration);
    if (ret < 0)
        return ret;
    printf("%s: duration=%f\n", filename, duration);
    sxplayer_release_frame(sxplayer_get_next_frame(s));
    sxplayer_free(&s);
    return 0;
}

static const char *filename = "/i/do/not/exist";

static void log_callback(void *arg, int level, const char *fmt, va_list vl)
{
    av_assert0(arg == filename);
    printf("fmt=%s level=%d\n", fmt, level);
}

static int run_notavail_file_test(void)
{
    struct sxplayer_ctx *s = sxplayer_create(filename);

    if (!s)
        return -1;
    sxplayer_set_log_callback(s, (void*)filename, log_callback);
    sxplayer_release_frame(sxplayer_get_frame(s, -1));
    sxplayer_release_frame(sxplayer_get_frame(s, 1.0));
    sxplayer_release_frame(sxplayer_get_frame(s, 3.0));
    sxplayer_free(&s);
    return 0;
}

int main(int ac, char **av)
{
    if (ac != 2 && ac != 3) {
        fprintf(stderr, "Usage: %s [-notest] <file>\n", av[0]);
        return -1;
    }

    if (test_duration(av[1 + (ac == 3)]) < 0)
        return -1;

    if (test_next_frame(av[1]) < 0)
        return -1;

    if (ac == 3 && !strcmp(av[1], "-notest"))
        return simple_pass_through(av[2]);

    if (run_notavail_file_test() < 0)
        return -1;

    if (run_tests(av[1], SXPLAYER_SELECT_VIDEO) < 0 ||
        run_tests(av[1], SXPLAYER_SELECT_AUDIO) < 0)
        return -1;

    printf("All tests OK\n");

    return 0;
}
