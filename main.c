#include <libavutil/common.h>
#include <float.h> // for DBL_MIN

#include "sfxmp.h"

#define N 4
#define NB_FRAMES (1<<(3*N))
#define SOURCE_FPS 25

#define PRE_FILL_TIME 3     /* try to pre fetch starting 3 seconds earlier */
#define POST_REQUEST_TIME 2 /* attempt to fetch the last frame for 2 more seconds */

static int test_frame(struct sfxmp_ctx *s,
                      double time, int *prev_frame_id, double *prev_time,
                      double visible_time, double start_time,
                      double skip, double trim_duration, int avselect)
{
    struct sfxmp_frame *frame = sfxmp_get_frame(s, time);

    if (avselect == SFXMP_SELECT_AUDIO) {
        // TODO
        if (frame) {
            const double playback_time = av_clipd(time, start_time, trim_duration < 0 ? DBL_MAX : start_time + trim_duration);
            const double diff = FFABS(playback_time - frame->ts);
            printf("AUDIO TEST FRAME %dx%d pt:%f ft:%f diff:%f\n", frame->width, frame->height, playback_time, frame->ts, diff);
        }
        sfxmp_release_frame(frame);
        return 0;
    }

    if (frame) {
        const uint32_t c = *(const uint32_t *)frame->data;
        const uint32_t c2 = ((const uint32_t *)frame->data)[1];
        const int r = c >> (N+16) & 0xf;
        const int g = c >> (N+ 8) & 0xf;
        const int b = c >> (N+ 0) & 0xf;

        const int frame_id = r<<(N*2) | g<<N | b;
        const double video_ts = frame_id * 1. / SOURCE_FPS;
        const double estimated_time_from_color = start_time + video_ts - skip;
        const double playback_time = av_clipd(time, start_time, trim_duration < 0 ? DBL_MAX : start_time + trim_duration);
        const double diff = FFABS(playback_time - estimated_time_from_color);

        if ((c == 0xff0000ff && c2 == 0xffffffff) /* this is for when ENABLE_DBG=1 in sfxmp.c */ ||
            (c == 0x00000000 && c2 == 0x00000000) /* this is for when ENABLE_DBG=0 in sfxmp.c */) {
            if (time >= visible_time) {
                fprintf(stderr, "got invisible frame even though it wasn't in visible time %f >= %f\n", time, visible_time);
                sfxmp_release_frame(frame);
                return -1;
            } else {
                printf("Got invisible time\n");
                *prev_time = time;
                sfxmp_release_frame(frame);
                return 0;
            }
        }

        printf("frame t=%f (pt=%f) -> %p color %08X => video_ts:%f, frame expected at t=%f [diff:%f]\n",
               time, playback_time, frame, c, video_ts, estimated_time_from_color, diff);

        if (diff > 2./SOURCE_FPS) {
            fprintf(stderr, "ERROR: frame diff is too big %f>%f\n", diff, 2./SOURCE_FPS);
            sfxmp_release_frame(frame);
            return -1;
        }

        if (frame_id == *prev_frame_id) {
            fprintf(stderr, "ERROR: Got a frame, but it has the same content as the previous one\n");
            sfxmp_release_frame(frame);
            return -1;
        }

        *prev_frame_id = frame_id;
    } else {

        if (time < *prev_time) {
            fprintf(stderr, "ERROR: time went backward but got no frame update\n");
            sfxmp_release_frame(frame);
            return -1;
        }

        if (time - *prev_time > 1.5/SOURCE_FPS) {
            if (time > start_time && time < start_time + trim_duration) {
                fprintf(stderr, "ERROR: the difference between current and previous time (%f) "
                        "is large enough to get a new frame, but got none\n", time - *prev_time);
                sfxmp_release_frame(frame);
                return -1;
            }
        }

        printf("frame t=%f: no frame\n", time);
    }
    *prev_time = time;
    sfxmp_release_frame(frame);
    return 0;
}

static int test_instant_gets(const char *filename, int avselect)
{
    int i, ret = 0;
    const double visible_time  = 15.2;
    const double start_time    = 19.8;
    const double skip          = 27.2;
    const double trim_duration = 67.1;

    const double instant_gets[] = {0.5, 17.2, 26.2, 38.4, 89.7, 97.6};

    printf("Test: %s\n", __FUNCTION__);

    for (i = 0; i < FF_ARRAY_ELEMS(instant_gets); i++) {
        int prev_frame_id = -1;
        double prev_time = DBL_MIN;

        struct sfxmp_ctx *s = sfxmp_create(filename, avselect,
                                           visible_time, start_time,
                                           skip, trim_duration,
                                           -1, -1, NULL);

        printf("Test instant get @ t=%f\n", instant_gets[i]);

        if (!s)
            return -1;

        ret = test_frame(s, instant_gets[i], &prev_frame_id, &prev_time,
                         visible_time, start_time, skip, trim_duration,
                         avselect);

        sfxmp_free(&s);

        if (ret < 0)
            break;
    }
    return ret;
}

static int test_seeks(const char *filename, int avselect)
{
    int i, ret = 0;
    const double visible_time  = 12.;
    const double start_time    = 21.;
    const double skip          = 45.;
    const double trim_duration = 120.;

    const double instant_gets[] = {32., 31., 31.2, 60.};

    struct sfxmp_ctx *s = sfxmp_create(filename, avselect,
                                       visible_time, start_time,
                                       skip, trim_duration,
                                       -1, -1, NULL);

    printf("Test: %s\n", __FUNCTION__);

    if (!s)
        return -1;

    for (i = 0; i < FF_ARRAY_ELEMS(instant_gets); i++) {
        int prev_frame_id = -1;
        double prev_time = DBL_MIN;

        ret = test_frame(s, instant_gets[i], &prev_frame_id, &prev_time,
                         visible_time, start_time, skip, trim_duration,
                         avselect);

        if (ret < 0)
            break;
    }

    sfxmp_free(&s);
    return ret;
}

static int test_full_run(const char *filename, int refresh_rate,
                         double visible_time, double start_time,
                         double skip, double trim_duration,
                         int avselect)
{
    int i, ret = 0, prev_frame_id = -1;
    double prev_time = DBL_MIN;
    const double request_start_time = FFMAX(start_time - PRE_FILL_TIME, 0);
    const double request_end_time   = start_time + trim_duration + POST_REQUEST_TIME;
    const double request_duration   = request_end_time - request_start_time;
    const int nb_calls = request_duration * refresh_rate;

    struct sfxmp_ctx *s = sfxmp_create(filename, avselect,
                                       visible_time, start_time,
                                       skip, trim_duration,
                                       -1, -1, NULL);

    printf("Test: %s\n", __FUNCTION__);

    if (!s)
        return -1;

    if (!filename)
        goto end;

    printf("test full run of %s [%dFPS]\n",
           filename, SOURCE_FPS /* FIXME: probe from file */);

    printf("    start_time:%f visible_time:%f skip:%f trim_duration:%f\n",
           start_time, visible_time, skip, trim_duration);

    printf("    request: %f->%f (duration:%f) @ %dHz => nb_calls:%d\n",
           request_start_time, request_end_time, request_duration,
           refresh_rate, nb_calls);

    for (i = 0; i < nb_calls; i++) {
        const double time = request_start_time + i / (double)refresh_rate;

        printf("TEST %d/%d\n", i + 1, nb_calls);

        ret = test_frame(s, time, &prev_frame_id, &prev_time,
                         visible_time, start_time,
                         skip, trim_duration,
                         avselect);
        if (ret < 0)
            break;
    }

end:
    sfxmp_free(&s);
    return ret;
}

static int run_tests(const char *filename, int avselect)
{
    if (test_seeks(filename, avselect) < 0)
        return 1;

    if (test_full_run("dummy", 0, 0, 0, 0, 0, avselect) < 0 ||
        test_full_run(filename, 30, 0, 0, 0, -1, avselect) < 0 ||
        test_full_run(filename, 10, 3.2, 5.4, 1.1, 18.6, avselect) < 0 ||
        test_full_run(filename, 10, 3.2, 5.4, 1.1, 18.6, avselect) < 0 ||
        test_full_run(filename, 60, 2.3, 4.5, 3.7, 12.2, avselect) < 0)
        return 1;

    if (test_instant_gets(filename, avselect) < 0)
        return 1;

    return 0;
}

int main(int ac, char **av)
{
    if (ac != 2) {
        fprintf(stderr, "Usage: %s <file>\n", av[0]);
        return 1;
    }

    if (run_tests(av[1], SFXMP_SELECT_VIDEO) < 0 ||
        run_tests(av[1], SFXMP_SELECT_AUDIO) < 0)
        return 1;

    printf("All tests OK\n");

    return 0;
}
