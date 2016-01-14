#define _GNU_SOURCE // pthread_setname_np on Linux
#include <pthread.h>

#include "sxplayer.h"
#include "internal.h"

static const struct {
    enum AVPixelFormat ff;
    enum sxplayer_pixel_format sx;
} pix_fmts_mapping[] = {
    {AV_PIX_FMT_VIDEOTOOLBOX, SXPLAYER_PIXFMT_VT},
    {AV_PIX_FMT_BGRA,         SXPLAYER_PIXFMT_BGRA},
    {AV_PIX_FMT_RGBA,         SXPLAYER_PIXFMT_RGBA},
};

enum AVPixelFormat pix_fmts_sx2ff(enum sxplayer_pixel_format pix_fmt)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(pix_fmts_mapping); i++)
        if (pix_fmts_mapping[i].sx == pix_fmt)
            return pix_fmts_mapping[i].ff;
    return AV_PIX_FMT_NONE;
}

enum sxplayer_pixel_format pix_fmts_ff2sx(enum AVPixelFormat pix_fmt)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(pix_fmts_mapping); i++)
        if (pix_fmts_mapping[i].ff == pix_fmt)
            return pix_fmts_mapping[i].sx;
    return -1;
}

void set_thread_name(const char *name)
{
#if __APPLE__
    pthread_setname_np(name);
#elif __linux__
    pthread_setname_np(pthread_self(), name);
#endif
}
