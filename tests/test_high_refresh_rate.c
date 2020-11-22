#include <stdio.h>
#include <stdlib.h>

#include <sxplayer.h>

int main(int ac, char **av)
{
    if (ac < 2) {
        fprintf(stderr, "Usage: %s <media.mkv> [use_pkt_duration]\n", av[0]);
        return -1;
    }

    const char *filename = av[1];
    const int use_pkt_duration = ac > 2 ? atoi(av[2]) : 0;

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
