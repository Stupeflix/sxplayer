#include <stdio.h>
#include <stdlib.h>

#include <sxplayer.h>

int main(int ac, char **av)
{
    if (ac < 2) {
        fprintf(stderr, "Usage: %s <image.jpg> [<use_pkt_duration>]\n", av[0]);
        return -1;
    }

    const char *filename = av[1];
    const int use_pkt_duration = ac > 2 ? atoi(av[2]) : 0;

    struct sxplayer_info info;
    struct sxplayer_ctx *s = sxplayer_create(filename);
    struct sxplayer_frame *f;

    if (!s)
        return -1;
    sxplayer_set_option(s, "skip", 3.0);
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
