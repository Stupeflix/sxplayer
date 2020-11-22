#include <stdio.h>
#include <stdlib.h>

#include <sxplayer.h>

static const char *fake_filename = "/i/do/not/exist";

static void log_callback(void *arg, int level, const char *filename, int ln,
                         const char *fn, const char *fmt, va_list vl)
{
    if (arg != fake_filename)
        abort();
    printf("level=%d filename=%s ln=%d fn=%s fmt=%s\n", level, filename, ln, fn, fmt);
}

int main(int ac, char **av)
{
    const int use_pkt_duration = ac > 1 ? atoi(av[1]) : 0;

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
