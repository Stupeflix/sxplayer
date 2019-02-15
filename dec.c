#include <stdio.h>

#include "sxplayer.h"

static int decode(const char *filename)
{
    struct sxplayer_ctx *s = sxplayer_create(filename);
    if (!s)
        return -1;

    sxplayer_set_option(s, "max_nb_packets", 8);
    sxplayer_set_option(s, "max_nb_frames", 1);
    sxplayer_set_option(s, "max_nb_sink", 1);
    sxplayer_set_option(s, "auto_hwaccel", 1);
    sxplayer_set_option(s, "stream_idx", 0);
    sxplayer_set_option(s, "vt_pix_fmt", "nv12");

    int i = 0;
    for (;;) {
        struct sxplayer_frame *frame = sxplayer_get_next_frame(s);
        if (!frame)
            break;
        printf("frame #%d / data:%p ts:%f %dx%d lz:%d sfxpixfmt:%d\n",
                i++, frame->data, frame->ts, frame->width, frame->height,
                frame->linesize, frame->pix_fmt);
        sxplayer_release_frame(frame);
    }

    printf("decoded: %d\n", i);
    sxplayer_free(&s);
    return 0;
}

int main(int ac, char **av)
{
    if (ac != 2) {
        fprintf(stderr, "Usage: %s <media>\n", av[0]);
        return -1;
    }
    return decode(av[1]);
}
