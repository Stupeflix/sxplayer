#include <stdio.h>
#include <stdlib.h>

#include <sxplayer.h>

int main(int ac, char **av)
{
    if (ac < 2) {
        fprintf(stderr, "Usage: %s <media.mkv> [<use_pkt_duration>]\n", av[0]);
        return -1;
    }

    const char *filename = av[1];
    const int use_pkt_duration = ac > 2 ? atoi(av[2]) : 0;

    int i = 0, ret = 0, r;
    struct sxplayer_ctx *s = sxplayer_create(filename);

    if (!s)
        return -1;

    sxplayer_set_option(s, "auto_hwaccel", 0);
    sxplayer_set_option(s, "use_pkt_duration", use_pkt_duration);

    for (r = 0; r < 2; r++) {
        printf("run #%d\n", r+1);
        for (;;) {
            struct sxplayer_frame *frame = sxplayer_get_next_frame(s);

            if (!frame) {
                printf("null frame\n");
                break;
            }
            printf("frame #%d / data:%p ts:%f %dx%d lz:%d sfxpixfmt:%d\n",
                   i++, frame->datap[0], frame->ts, frame->width, frame->height,
                   frame->linesizep[0], frame->pix_fmt);

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
