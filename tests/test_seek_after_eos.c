#include <stdio.h>
#include <stdlib.h>

#include <sxplayer.h>

#define FLAG_SKIP          (1<<0)
#define FLAG_TRIM_DURATION (1<<1)
#define FLAG_AUDIO         (1<<2)

int main(int ac, char **av)
{
    if (ac < 3) {
        fprintf(stderr, "Usage: %s <media.mkv> <flags> [<use_pkt_duration>]\n", av[0]);
        return -1;
    }

    const char *filename = av[1];
    const int flags = atoi(av[2]);
    const int use_pkt_duration = ac > 3 ? atoi(av[3]) : 0;

    int ret = 0, nb_frames = 0;
    struct sxplayer_ctx *s = sxplayer_create(filename);
    struct sxplayer_frame *frame = NULL;
    static const float ts[] = { 0.0, 0.5, 7.65 };

    if (!s)
        return -1;

    const double skip     = (flags & FLAG_SKIP)          ? 60.0 : 0.0;
    const double duration = (flags & FLAG_TRIM_DURATION) ? 10.0 : -1.0;
    const int avselect    = (flags & FLAG_AUDIO)         ? SXPLAYER_SELECT_AUDIO : SXPLAYER_SELECT_VIDEO;

    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "avselect", avselect);
    sxplayer_set_option(s, "audio_texture", 0);
    sxplayer_set_option(s, "skip", skip);
    sxplayer_set_option(s, "trim_duration", duration);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);

    printf("run #1 (avselect=%d duration=%f)\n", avselect, duration);
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
