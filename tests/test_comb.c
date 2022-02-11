#include <stdio.h>
#include <libavutil/common.h>
#include <float.h> // for DBL_MAX

#include <sxplayer.h>

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

#define FLAG_START_TIME    (1<<0)
#define FLAG_END_TIME      (1<<1)
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

#define TESTVAL_START_TIME  7.12
#define TESTVAL_END_TIME   60.43

static int check_frame(struct sxplayer_frame *f, double t, int opt_test_flags)
{
    const double start_time    = (opt_test_flags & FLAG_START_TIME) ? TESTVAL_START_TIME :  0;
    const double end_time      = (opt_test_flags & FLAG_END_TIME)   ? TESTVAL_END_TIME   : -1;
    const double playback_time = av_clipd(t, 0, end_time < 0 ? DBL_MAX : end_time);

    const double frame_ts = f ? f->ts : -1;
    const double estimated_time_from_ts = frame_ts - start_time;
    const double diff_ts = fabs(playback_time - estimated_time_from_ts);

    if (!f) {
        fprintf(stderr, "no frame obtained for t=%f\n", t);
        return -1;
    }

    if (!(opt_test_flags & FLAG_AUDIO)) {
        const uint32_t c = *(const uint32_t *)f->datap[0];
        const int r = c >> (N+16) & 0xf;
        const int g = c >> (N+ 8) & 0xf;
        const int b = c >> (N+ 0) & 0xf;
        const int frame_id = r<<(N*2) | g<<N | b;

        const double video_ts = frame_id * 1. / SOURCE_FPS;
        const double estimated_time_from_color = video_ts - start_time;
        const double diff_color = fabs(playback_time - estimated_time_from_color);

        if (diff_color > 1./SOURCE_FPS) {
            fprintf(stderr, "requested t=%f (clipped to %f with end_time=%f),\n"
                    "got video_ts=%f (frame id #%d), corresponding to t=%f (with start_time=%f)\n"
                    "diff_color: %f\n",
                    t, playback_time, end_time,
                    video_ts, frame_id, estimated_time_from_color, start_time,
                    diff_color);
            return -1;
        }
    }
    if (diff_ts > 1./SOURCE_FPS) {
        fprintf(stderr, "requested t=%f (clipped to %f with end_time=%f),\n"
                "got frame_ts=%f, corresponding to t=%f (with start_time=%f)\n"
                "diff_ts: %f\n",
                t, playback_time, end_time,
                frame_ts, estimated_time_from_ts, start_time,
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
    if (opt_test_flags & FLAG_START_TIME)    printf("start_time-");
    if (opt_test_flags & FLAG_END_TIME)      printf("end_time-");
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

    if (opt_test_flags & FLAG_START_TIME) sxplayer_set_option(s, "start_time", TESTVAL_START_TIME);
    if (opt_test_flags & FLAG_END_TIME)   sxplayer_set_option(s, "end_time",   TESTVAL_END_TIME);
    if (opt_test_flags & FLAG_AUDIO)      sxplayer_set_option(s, "avselect",   SXPLAYER_SELECT_AUDIO);

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

int main(int ac, char **av)
{
    if (ac < 3) {
        fprintf(stderr, "Usage: %s <media.mkv> <flags> [<use_pkt_duration>]\n", av[0]);
        return -1;
    }

    const char *filename = av[1];
    const int flags = atoi(av[2]);
    const int use_pkt_duration = ac > 3 ? atoi(av[3]) : 0;

    int ret = 0;
    uint64_t comb = 0;

    for (;;) {
        comb = get_next_comb(comb);
        if (comb == EOA)
            break;
        ret = exec_comb(filename, comb, flags, use_pkt_duration);
        if (ret < 0) {
            fprintf(stderr, "test failed\n");
            break;
        }
    }
    return ret;
}
