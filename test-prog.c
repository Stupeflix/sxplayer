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
#include <float.h> // for DBL_MAX

#include "sxplayer.h"

#define BITS_PER_ACTION 4

enum action {
    EOA,                    // end of actions
    ACTION_PREFETCH,        // request a prefetch
    ACTION_FETCH_INFO,      // fetch the media info
    ACTION_START,           // request a frame at t=0
    ACTION_MIDDLE,          // request a few frames in the middle
    ACTION_END,             // request the last frame post end
    NB_ACTIONS
};

static int action_prefetch(struct sxplayer_ctx *s, int opt_test_flags)
{
    return sxplayer_start(s);
}

#define FLAG_SKIP          (1<<0)
#define FLAG_TRIM_DURATION (1<<1)
#define FLAG_AUDIO         (1<<2)

static int action_fetch_info(struct sxplayer_ctx *s, int opt_test_flags)
{
    struct sxplayer_info info;
    int ret = sxplayer_get_info(s, &info);
    if (ret < 0)
        return ret;
    if (opt_test_flags & FLAG_AUDIO) {
        if (info.width || info.height)
            return -1;
    } else {
        if (info.width != 16 || info.height != 16)
            return -1;
    }
    return 0;
}

#define N 4
#define SOURCE_FPS 25
#define SOURCE_SPF  1024    /* samples per frame, must match AUDIO_NBSAMPLES */
#define SOURCE_FREQ 44100

#define TESTVAL_SKIP           7.12
#define TESTVAL_TRIM_DURATION 53.43

static int check_frame(struct sxplayer_frame *f, double t, int opt_test_flags)
{
    const double skip          = (opt_test_flags & FLAG_SKIP)          ? TESTVAL_SKIP          :  0;
    const double trim_duration = (opt_test_flags & FLAG_TRIM_DURATION) ? TESTVAL_TRIM_DURATION : -1;
    const double playback_time = av_clipd(t, 0, trim_duration < 0 ? DBL_MAX : trim_duration);

    const double frame_ts = f ? f->ts : -1;
    const double estimated_time_from_ts = frame_ts - skip;
    const double diff_ts = fabs(playback_time - estimated_time_from_ts);

    if (!f) {
        fprintf(stderr, "no frame obtained for t=%f\n", t);
        return -1;
    }

    if (!(opt_test_flags & FLAG_AUDIO)) {
        const uint32_t c = *(const uint32_t *)f->data;
        const int r = c >> (N+16) & 0xf;
        const int g = c >> (N+ 8) & 0xf;
        const int b = c >> (N+ 0) & 0xf;
        const int frame_id = r<<(N*2) | g<<N | b;

        const double video_ts = frame_id * 1. / SOURCE_FPS;
        const double estimated_time_from_color = video_ts - skip;
        const double diff_color = fabs(playback_time - estimated_time_from_color);

        if (diff_color > 1./SOURCE_FPS) {
            fprintf(stderr, "requested t=%f (clipped to %f with trim_duration=%f),\n"
                    "got video_ts=%f (frame id #%d), corresponding to t=%f (with skip=%f)\n"
                    "diff_color: %f\n",
                    t, playback_time, trim_duration,
                    video_ts, frame_id, estimated_time_from_color, skip,
                    diff_color);
            return -1;
        }
    }
    if (diff_ts > 1./SOURCE_FPS) {
        fprintf(stderr, "requested t=%f (clipped to %f with trim_duration=%f),\n"
                "got frame_ts=%f, corresponding to t=%f (with skip=%f)\n"
                "diff_ts: %f\n",
                t, playback_time, trim_duration,
                frame_ts, estimated_time_from_ts, skip,
                diff_ts);
        return -1;
    }
    return 0;
}

static int action_start(struct sxplayer_ctx *s, int opt_test_flags)
{
    int ret;
    struct sxplayer_frame *frame = sxplayer_get_frame(s, 0);

    if ((ret = check_frame(frame, 0, opt_test_flags)) < 0)
        return ret;
    sxplayer_release_frame(frame);
    return 0;
}

static int action_middle(struct sxplayer_ctx *s, int opt_test_flags)
{
    int ret;
    struct sxplayer_frame *f0 = sxplayer_get_frame(s, 30.0);
    struct sxplayer_frame *f1 = sxplayer_get_frame(s, 30.1);
    struct sxplayer_frame *f2 = sxplayer_get_frame(s, 30.2);
    struct sxplayer_frame *f3 = sxplayer_get_frame(s, 15.0);
    struct sxplayer_frame *f4 = sxplayer_get_next_frame(s);
    struct sxplayer_frame *f5 = sxplayer_get_next_frame(s);
    const double increment = opt_test_flags & FLAG_AUDIO ? SOURCE_SPF/(float)SOURCE_FREQ
                                                         : 1./SOURCE_FPS;

    if ((ret = check_frame(f0, 30.0,               opt_test_flags)) < 0 ||
        (ret = check_frame(f1, 30.1,               opt_test_flags)) < 0 ||
        (ret = check_frame(f2, 30.2,               opt_test_flags)) < 0 ||
        (ret = check_frame(f3, 15.0,               opt_test_flags)) < 0 ||
        (ret = check_frame(f4, 15.0 + 1*increment, opt_test_flags)) < 0 ||
        (ret = check_frame(f5, 15.0 + 2*increment, opt_test_flags)) < 0)
        return ret;

    sxplayer_release_frame(f0);
    sxplayer_release_frame(f5);
    sxplayer_release_frame(f1);
    sxplayer_release_frame(f4);
    sxplayer_release_frame(f2);
    sxplayer_release_frame(f3);

    f0 = sxplayer_get_next_frame(s);
    f1 = sxplayer_get_frame(s, 16.0);
    f2 = sxplayer_get_frame(s, 16.001);

    if ((ret = check_frame(f0, 15.0 + 3*increment, opt_test_flags)) < 0 ||
        (ret = check_frame(f1, 16.0,               opt_test_flags)) < 0)
        return ret;

    if (f2) {
        fprintf(stderr, "got f2\n");
        return -1;
    }

    sxplayer_release_frame(f1);
    sxplayer_release_frame(f0);

    return 0;
}

static int action_end(struct sxplayer_ctx *s, int opt_test_flags)
{
    struct sxplayer_frame *f;

    f = sxplayer_get_frame(s, 999999.0);
    if (!f)
        return -1;
    sxplayer_release_frame(f);

    f = sxplayer_get_frame(s, 99999.0);
    if (f) {
        sxplayer_release_frame(f);
        return -1;
    }

    return 0;
}

static const struct {
    const char *name;
    int (*func)(struct sxplayer_ctx *s, int opt_test_flags);
} actions_desc[] = {
    [ACTION_PREFETCH]   = {"prefetch",  action_prefetch},
    [ACTION_FETCH_INFO] = {"fetchinfo", action_fetch_info},
    [ACTION_START]      = {"start",     action_start},
    [ACTION_MIDDLE]     = {"middle",    action_middle},
    [ACTION_END]        = {"end",       action_end},
};

#define GET_ACTION(c, id) ((c) >> ((id)*BITS_PER_ACTION) & ((1<<BITS_PER_ACTION)-1))

static void print_comb_name(uint64_t comb, int opt_test_flags)
{
    printf(":: test-%s-", (opt_test_flags & FLAG_AUDIO) ? "audio" : "video");
    if (opt_test_flags & FLAG_SKIP)          printf("skip-");
    if (opt_test_flags & FLAG_TRIM_DURATION) printf("trimdur-");
    for (int i = 0; i < NB_ACTIONS; i++) {
        const int action = GET_ACTION(comb, i);
        if (!action)
            break;
        printf("%s%s", i ? "-" : "", actions_desc[action].name);
    }
    printf(" (comb=0x%"PRIx64")\n", comb);
}

static int exec_comb(const char *filename, uint64_t comb, int opt_test_flags, int use_pkt_duration)
{
    int ret = 0;
    struct sxplayer_ctx *s = sxplayer_create(filename);
    if (!s)
        return -1;

    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);

    print_comb_name(comb, opt_test_flags);

    if (opt_test_flags & FLAG_SKIP)          sxplayer_set_option(s, "skip",          TESTVAL_SKIP);
    if (opt_test_flags & FLAG_TRIM_DURATION) sxplayer_set_option(s, "trim_duration", TESTVAL_TRIM_DURATION);
    if (opt_test_flags & FLAG_AUDIO)         sxplayer_set_option(s, "avselect",      SXPLAYER_SELECT_AUDIO);

    for (int i = 0; i < NB_ACTIONS; i++) {
        const int action = GET_ACTION(comb, i);
        if (!action)
            break;
        ret = actions_desc[action].func(s, opt_test_flags);
        if (ret < 0)
            break;
    }

    sxplayer_free(&s);
    return ret;
}

static int has_dup(uint64_t comb)
{
    uint64_t actions = 0;

    for (int i = 0; i < NB_ACTIONS; i++) {
        const int action = GET_ACTION(comb, i);
        if (!action)
            break;
        if (actions & (1<<action))
            return 1;
        actions |= 1 << action;
    }
    return 0;
}

static uint64_t get_next_comb(uint64_t comb)
{
    int i = 0, need_inc = 1;
    uint64_t ret = 0;

    for (;;) {
        int action = GET_ACTION(comb, i);
        if (i == NB_ACTIONS)
            return EOA;
        if (!action && !need_inc)
            break;
        if (need_inc) {
            action++;
            if (action == NB_ACTIONS)
                action = 1; // back to first action
            else
                need_inc = 0;
        }
        ret |= action << (i*BITS_PER_ACTION);
        i++;
    }
    if (has_dup(ret))
        return get_next_comb(ret);
    return ret;
}

static int run_tests_all_combs(const char *filename, int opt_test_flags, int use_pkt_duration)
{
    int ret = 0;
    uint64_t comb = 0;

    for (;;) {
        comb = get_next_comb(comb);
        if (comb == EOA)
            break;
        ret = exec_comb(filename, comb, opt_test_flags, use_pkt_duration);
        if (ret < 0) {
            fprintf(stderr, "test failed\n");
            break;
        }
    }
    return ret;
}

static int run_image_test(const char *filename, int use_pkt_duration)
{
    struct sxplayer_info info;
    struct sxplayer_ctx *s = sxplayer_create(filename);
    struct sxplayer_frame *f;

    if (!s)
        return -1;
    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);
    f = sxplayer_get_frame(s, 53.0);
    if (!f) {
        fprintf(stderr, "didn't get an image\n");
        return -1;
    }
    sxplayer_release_frame(f);

    if (sxplayer_get_info(s, &info) < 0) {
        fprintf(stderr, "can not fetch image info\n");
    }
    if (info.width != 480 || info.height != 640) {
        fprintf(stderr, "image isn't the expected size\n");
        return -1;
    }

    f = sxplayer_get_frame(s, 12.3);
    if (f) {
        sxplayer_release_frame(f);
        fprintf(stderr, "we got a new frame even though the source is an image\n");
        return -1;
    }

    sxplayer_free(&s);
    return 0;
}

static int run_image_seek_test(const char *filename, int use_pkt_duration)
{
    struct sxplayer_ctx *s = sxplayer_create(filename);
    struct sxplayer_frame *f;

    if (!s)
        return -1;
    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);
    sxplayer_seek(s, 10.2);
    f = sxplayer_get_frame(s, 10.5);
    if (!f) {
        fprintf(stderr, "didn't get first image\n");
        return -1;
    }
    sxplayer_release_frame(f);

    sxplayer_free(&s);
    return 0;
}

static int test_next_frame(const char *filename, int use_pkt_duration)
{
    int i = 0, ret = 0, r;
    struct sxplayer_ctx *s = sxplayer_create(filename);

    if (!s)
        return -1;

    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);

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

    if (i != 8192) {
        fprintf(stderr, "decoded %d/8192 expected frames\n", i);
        ret = -1;
    }

    return ret;
}

static int run_audio_test(const char *filename, int use_pkt_duration)
{
    int i = 0, ret = 0, r, smp = 0;
    struct sxplayer_ctx *s = sxplayer_create(filename);

    if (!s)
        return -1;

    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);
    sxplayer_set_option(s, "avselect", SXPLAYER_SELECT_AUDIO);
    sxplayer_set_option(s, "audio_texture", 0);

    for (r = 0; r < 2; r++) {
        printf("Test: %s run #%d\n", __FUNCTION__, r+1);
        for (;;) {
            struct sxplayer_frame *frame = sxplayer_get_next_frame(s);

            if (!frame) {
                printf("null frame\n");
                break;
            }
            printf("frame #%d / data:%p ts:%f nb_samples:%d sfxsmpfmt:%d\n",
                   i++, frame->data, frame->ts, frame->nb_samples, frame->pix_fmt);
            smp += frame->nb_samples;

            sxplayer_release_frame(frame);
        }
    }

    sxplayer_free(&s);

    if (smp != 15876000) {
        fprintf(stderr, "decoded %d/15876000 expected samples\n", smp);
        ret = -1;
    }

    return ret;
}

static int run_audio_seek_test(const char *filename, int use_pkt_duration)
{
    int ret = 0;
    double last_ts = 0.0;
    struct sxplayer_ctx *s = sxplayer_create(filename);
    struct sxplayer_frame *frame = NULL;

    if (!s)
        return -1;

    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "avselect", SXPLAYER_SELECT_AUDIO);
    sxplayer_set_option(s, "audio_texture", 0);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);

    printf("Test: %s run #1\n", __FUNCTION__);
    for (int i = 0; i < 10; i++) {
        frame = sxplayer_get_next_frame(s);

        if (!frame) {
            fprintf(stderr, "got unexpected null frame\n");
            sxplayer_free(&s);
            return -1;
        }
        printf("frame #%d / data:%p ts:%f nb_samples:%d sfxsmpfmt:%d\n",
                i, frame->data, frame->ts, frame->nb_samples, frame->pix_fmt);
        last_ts = frame->ts;

        sxplayer_release_frame(frame);
    }

    sxplayer_seek(s, last_ts);

    frame = sxplayer_get_next_frame(s);
    if (!frame) {
        fprintf(stderr, "expected frame->ts=%f got null frame\n", last_ts);
        ret = -1;
    } else if (frame->ts != last_ts) {
        fprintf(stderr, "expected frame->ts=%f got frame->ts=%f\n", last_ts, frame->ts);
        ret = -1;
    }
    sxplayer_release_frame(frame);
    sxplayer_free(&s);

    return ret;
}

static int run_seek_test_after_eos(const char *filename, int avselect, double skip, double duration, int use_pkt_duration)
{
    int ret = 0, nb_frames = 0;
    struct sxplayer_ctx *s = sxplayer_create(filename);
    struct sxplayer_frame *frame = NULL;
    static const float ts[] = { 0.0, 0.5, 7.65 };

    if (!s)
        return -1;

    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "avselect", avselect);
    sxplayer_set_option(s, "audio_texture", 0);
    sxplayer_set_option(s, "skip", skip);
    sxplayer_set_option(s, "trim_duration", duration);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);

    printf("Test: %s run #1 (avselect=%d duration=%f)\n", __FUNCTION__, avselect, duration);
    for (;;) {
        frame = sxplayer_get_next_frame(s);
        if (!frame) {
            break;
        }
        sxplayer_release_frame(frame);
        nb_frames++;
    }

    sxplayer_free(&s);

    for (int k = 0; k < 4; k++) {
        for (int j = 0; j < sizeof(ts)/sizeof(*ts); j++) {
            s = sxplayer_create(filename);
            if (!s)
                return -1;

            sxplayer_set_option(s, "auto_hwaccel", 0);
            sxplayer_set_option(s, "avselect", avselect);
            sxplayer_set_option(s, "audio_texture", 0);
            sxplayer_set_option(s, "skip", skip);
            sxplayer_set_option(s, "trim_duration", duration);

            for (int i = 0; i < nb_frames; i++) {
                frame = sxplayer_get_next_frame(s);
                if (!frame) {
                    fprintf(stderr, "unexpected null frame before EOS\n");
                    ret = -1;
                    goto done;
                }
                sxplayer_release_frame(frame);
            }

            if (k == 0) {
                sxplayer_seek(s, ts[j]);
                frame = sxplayer_get_next_frame(s);
                if (!frame) {
                    fprintf(stderr, "unexpected null frame from sxplayer_get_next_frame() after seeking at %f\n", ts[j]);
                    ret = -1;
                    goto done;
                }
            } else if (k == 1) {
                frame = sxplayer_get_frame(s, ts[j]);
                if (!frame) {
                    fprintf(stderr, "unexpected null frame from sxplayer_get_frame() after seeking at %f\n", ts[j]);
                    ret = -1;
                    goto done;
                }
            } else if (k == 2) {
                frame = sxplayer_get_frame(s, 1000.0);
                if (frame) {
                    fprintf(stderr, "unexpected frame at %f with ts=%f\n", 1000.0f, frame->ts);
                    ret = -1;
                    sxplayer_release_frame(frame);
                    goto done;
                }
            } else if (k == 3) {
                frame = NULL;
            }
            sxplayer_release_frame(frame);
            sxplayer_free(&s);
        }
    }

done:
    sxplayer_free(&s);

    return ret;
}

static const char *fake_filename = "/i/do/not/exist";

static void log_callback(void *arg, int level, const char *filename, int ln,
                         const char *fn, const char *fmt, va_list vl)
{
    av_assert0(arg == fake_filename);
    printf("level=%d filename=%s ln=%d fn=%s fmt=%s\n", level, filename, ln, fn, fmt);
}

static int run_notavail_file_test(int use_pkt_duration)
{
    struct sxplayer_ctx *s = sxplayer_create(fake_filename);

    if (!s)
        return -1;
    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);
    sxplayer_set_log_callback(s, (void*)fake_filename, log_callback);
    sxplayer_release_frame(sxplayer_get_frame(s, -1));
    sxplayer_release_frame(sxplayer_get_frame(s, 1.0));
    sxplayer_release_frame(sxplayer_get_frame(s, 3.0));
    sxplayer_free(&s);
    return 0;
}

static int run_misc_events(const char *filename, int use_pkt_duration)
{
    struct sxplayer_ctx *s = sxplayer_create(filename);
    struct sxplayer_frame *f;

    if (!s)
        return -1;
    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);
    sxplayer_seek(s, 12.7);
    sxplayer_seek(s, 21.0);
    sxplayer_seek(s, 5.3);
    sxplayer_start(s);
    sxplayer_start(s);
    sxplayer_seek(s, 15.3);
    sxplayer_stop(s);
    sxplayer_start(s);
    sxplayer_stop(s);
    sxplayer_start(s);
    sxplayer_seek(s, 7.2);
    sxplayer_start(s);
    sxplayer_stop(s);
    sxplayer_seek(s, 82.9);
    f = sxplayer_get_frame(s, 83.5);
    if (!f) {
        sxplayer_free(&s);
        return -1;
    }
    sxplayer_release_frame(f);
    sxplayer_stop(s);
    f = sxplayer_get_frame(s, 83.5);
    if (!f) {
        sxplayer_free(&s);
        return -1;
    }
    sxplayer_free(&s);
    sxplayer_release_frame(f);
    return 0;
}

static const int tests_flags[] = {
    0,
               FLAG_SKIP,
                         FLAG_TRIM_DURATION,
               FLAG_SKIP|FLAG_TRIM_DURATION,
    FLAG_AUDIO,
    FLAG_AUDIO|FLAG_SKIP,
    FLAG_AUDIO|          FLAG_TRIM_DURATION,
    FLAG_AUDIO|FLAG_SKIP|FLAG_TRIM_DURATION,
};

static int test_high_refresh_rate(const char *filename, int use_pkt_duration)
{
    struct sxplayer_ctx *s = sxplayer_create(filename);
    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);
    struct sxplayer_frame *f;
    const double t = 1/60.;

    if (!s)
        return -1;

    f = sxplayer_get_frame(s, 0.);
    if (!f)
        return -1;
    sxplayer_release_frame(f);
    f = sxplayer_get_frame(s, t);
    if (f && f->ts > t) {
        fprintf(stderr, "unexpected frame at %f with ts=%f\n", t, f->ts);
        sxplayer_release_frame(f);
        return -1;
    }
    sxplayer_free(&s);
    return 0;
}

static int run_test_ms(const char *filename, int use_pkt_duration)
{
    struct sxplayer_ctx *s1 = sxplayer_create(filename);
    struct sxplayer_ctx *s2 = sxplayer_create(filename);
    struct sxplayer_frame *f1, *f2;

    if (!s1 || !s2)
        return -1;

    sxplayer_set_option(s1, "auto_hwaccel", 0);
    sxplayer_set_option(s1, "use_pkt_duration", use_pkt_duration);
    sxplayer_set_option(s2, "auto_hwaccel", 0);
    sxplayer_set_option(s2, "use_pkt_duration", use_pkt_duration);
    f1 = sxplayer_get_frame(s1, 3.0);
    f2 = sxplayer_get_frame_ms(s2, 3*1000000);
    if (!f1 || !f2)
        return -1;

    if (f1->ts != f2->ts) {
        fprintf(stderr, "%g != %g\n", f1->ts, f2->ts);
        return -1;
    }

    if (f1->ms != f2->ms) {
        fprintf(stderr, "%"PRId64" != %"PRId64"\n", f1->ms, f2->ms);
        return -1;
    }

    sxplayer_release_frame(f1);
    sxplayer_release_frame(f2);
    sxplayer_free(&s1);
    sxplayer_free(&s2);
    return 0;
}

static int run_tests(int ac, char **av, int use_pkt_duration)
{
    if (test_high_refresh_rate(av[1], use_pkt_duration) < 0)
        return -1;

    if (run_image_test(av[2], use_pkt_duration) < 0)
        return -1;

    if (run_image_seek_test(av[2], use_pkt_duration) < 0)
        return -1;

    if (run_notavail_file_test(use_pkt_duration) < 0)
        return -1;

    if (test_next_frame(av[1], use_pkt_duration) < 0)
        return -1;

    if (run_misc_events(av[1], use_pkt_duration) < 0)
        return -1;

    if (run_misc_events(av[2], use_pkt_duration) < 0)
        return -1;

    if (run_audio_test(av[1], use_pkt_duration) < 0)
        return -1;

    if (run_audio_seek_test(av[1], use_pkt_duration) < 0)
        return -1;

    if (run_seek_test_after_eos(av[1], SXPLAYER_SELECT_AUDIO,  0.0, -1.0, use_pkt_duration) ||
        run_seek_test_after_eos(av[1], SXPLAYER_SELECT_AUDIO,  0.0, 10.0, use_pkt_duration) ||
        run_seek_test_after_eos(av[1], SXPLAYER_SELECT_AUDIO, 60.0, -1.0, use_pkt_duration) ||
        run_seek_test_after_eos(av[1], SXPLAYER_SELECT_AUDIO, 60.0, 10.0, use_pkt_duration) ||
        run_seek_test_after_eos(av[1], SXPLAYER_SELECT_VIDEO,  0.0, -1.0, use_pkt_duration) ||
        run_seek_test_after_eos(av[1], SXPLAYER_SELECT_VIDEO,  0.0, 10.0, use_pkt_duration) ||
        run_seek_test_after_eos(av[1], SXPLAYER_SELECT_VIDEO, 60.0, -1.0, use_pkt_duration) ||
        run_seek_test_after_eos(av[1], SXPLAYER_SELECT_VIDEO, 60.0, 10.0, use_pkt_duration))
        return -1;

    if (run_test_ms(av[1], use_pkt_duration) < 0)
        return -1;

    for (int i = 0; i < sizeof(tests_flags)/sizeof(*tests_flags); i++)
        if (run_tests_all_combs(av[1], tests_flags[i], use_pkt_duration) < 0)
            return -1;

    printf("All tests OK (use_pkt_duration=%d)\n", use_pkt_duration);

    return 0;
}

int main(int ac, char **av)
{
    if (ac != 3) {
        fprintf(stderr, "Usage: %s <media.mkv> <image.jpg>\n", av[0]);
        return -1;
    }

    if (run_tests(ac, av, 0) < 0)
        return -1;

    if (run_tests(ac, av, 1) < 0)
        return -1;

    return 0;
}
