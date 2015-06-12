#ifndef SFXMP_INTERNAL_H
#define SFXMP_INTERNAL_H

#include <stdio.h>

#define ENABLE_DBG 0

#define DBG_SFXMP(mod, ...) do { printf("[sfxmp:"mod"] " __VA_ARGS__); fflush(stdout); } while (0)
#if ENABLE_DBG
# define DBG(mod, ...) DBG_SFXMP(mod, __VA_ARGS__)
#else
/* Note: this could be replaced by a "while(0)" but it wouldn't test the
 * compilation of the printf format, so we use this more complex form. */
# define DBG(mod, ...) do { if (0) DBG_SFXMP(mod, __VA_ARGS__); } while (0)
#endif

struct hwaccel_ctx {
    void *internal_ctx;             // only known by the acceleration code
    enum AVPixelFormat out_pix_fmt; // usable (non accelerated) pixel format
};

struct hwaccel {
    int (*decoder_init)(AVCodecContext *s);
    int (*get_frame)(AVCodecContext *s, AVFrame *frame);
    void (*uninit)(AVCodecContext *s);
    enum AVPixelFormat pix_fmt;
};

#endif
