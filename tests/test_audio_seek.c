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

    for (int i = 0; i < 10; i++) {
        frame = sxplayer_get_next_frame(s);

        if (!frame) {
            fprintf(stderr, "got unexpected null frame\n");
            sxplayer_free(&s);
            return -1;
        }
        printf("frame #%d / data:%p ts:%f nb_samples:%d sfxsmpfmt:%d\n",
                i, frame->datap[0], frame->ts, frame->nb_samples, frame->pix_fmt);
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
