/*
 * This file is part of sxplayer.
 *
 * Copyright (c) 2015 Stupeflix
 *
 * sxplayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * sxplayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with sxplayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define _GNU_SOURCE // pthread_setname_np on Linux
#include <pthread.h>

#include "sxplayer.h"
#include "internal.h"

static const struct {
    enum AVPixelFormat ff;
    enum sxplayer_pixel_format sx;
} pix_fmts_mapping[] = {
#if LIBAVCODEC_VERSION_INT >= MEDIACODEC_HWACCEL_VERSION_INT
    {AV_PIX_FMT_MEDIACODEC,   SXPLAYER_PIXFMT_MEDIACODEC},
#else
    {AV_PIX_FMT_RGBA,         SXPLAYER_PIXFMT_MEDIACODEC},
#endif
    {AV_PIX_FMT_VIDEOTOOLBOX, SXPLAYER_PIXFMT_VT},
    {AV_PIX_FMT_BGRA,         SXPLAYER_PIXFMT_BGRA},
    {AV_PIX_FMT_RGBA,         SXPLAYER_PIXFMT_RGBA},
};

static const struct {
    enum AVSampleFormat ff;
    enum sxplayer_pixel_format sx;
} smp_fmts_mapping[] = {
    {AV_SAMPLE_FMT_FLT,       SXPLAYER_SMPFMT_FLT},
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

enum sxplayer_pixel_format smp_fmts_ff2sx(enum AVSampleFormat smp_fmt)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(smp_fmts_mapping); i++)
        if (smp_fmts_mapping[i].ff == smp_fmt)
            return smp_fmts_mapping[i].sx;
    return -1;
}

void set_thread_name(const char *name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__) && defined(__GLIBC__)
    pthread_setname_np(pthread_self(), name);
#endif
}

void update_dimensions(int *width, int *height, int max_pixels)
{
    if (max_pixels) {
        const int w = *width;
        const int h = *height;
        const int t = w * h;
        if (t > max_pixels) {
            const double f = sqrt((double)max_pixels / t);
            *width  = (int)(w * f) & ~1;
            *height = (int)(h * f) & ~1;
        }
    }
}
