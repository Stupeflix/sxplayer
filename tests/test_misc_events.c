#include <stdio.h>
#include <stdlib.h>

#include <sxplayer.h>

int main(int ac, char **av)
{
    if (ac < 2) {
        fprintf(stderr, "Usage: %s <media> [<use_pkt_duration>]\n", av[0]);
        return -1;
    }

    const char *filename = av[1];
    const int use_pkt_duration = ac > 2 ? atoi(av[2]) : 0;

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
