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

#include "sxplayer.h"
#include "internal.h"
#include "pthread_compat.h"

static const struct {
    enum AVPixelFormat ff;
    enum sxplayer_pixel_format sx;
} pix_fmts_mapping[] = {
    {AV_PIX_FMT_MEDIACODEC,   SXPLAYER_PIXFMT_MEDIACODEC},
    {AV_PIX_FMT_VAAPI,        SXPLAYER_PIXFMT_VAAPI},
    {AV_PIX_FMT_VIDEOTOOLBOX, SXPLAYER_PIXFMT_VT},
    {AV_PIX_FMT_BGRA,         SXPLAYER_PIXFMT_BGRA},
    {AV_PIX_FMT_RGBA,         SXPLAYER_PIXFMT_RGBA},
    {AV_PIX_FMT_NV12,         SXPLAYER_PIXFMT_NV12},
    {AV_PIX_FMT_YUV420P,      SXPLAYER_PIXFMT_YUV420P},
    {AV_PIX_FMT_YUVJ420P,     SXPLAYER_PIXFMT_YUV420P},
    {AV_PIX_FMT_YUV422P,      SXPLAYER_PIXFMT_YUV422P},
    {AV_PIX_FMT_YUVJ422P,     SXPLAYER_PIXFMT_YUV422P},
    {AV_PIX_FMT_YUV444P,      SXPLAYER_PIXFMT_YUV444P},
    {AV_PIX_FMT_YUVJ444P,     SXPLAYER_PIXFMT_YUV444P},
    {AV_PIX_FMT_P010LE,       SXPLAYER_PIXFMT_P010LE},
    {AV_PIX_FMT_YUV420P10LE,  SXPLAYER_PIXFMT_YUV420P10LE},
    {AV_PIX_FMT_YUV422P10LE,  SXPLAYER_PIXFMT_YUV422P10LE},
    {AV_PIX_FMT_YUV444P10LE,  SXPLAYER_PIXFMT_YUV444P10LE},
};

static const struct {
    enum AVSampleFormat ff;
    enum sxplayer_pixel_format sx;
} smp_fmts_mapping[] = {
    {AV_SAMPLE_FMT_FLT,       SXPLAYER_SMPFMT_FLT},
};

enum AVPixelFormat sxpi_pix_fmts_sx2ff(enum sxplayer_pixel_format pix_fmt)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(pix_fmts_mapping); i++)
        if (pix_fmts_mapping[i].sx == pix_fmt)
            return pix_fmts_mapping[i].ff;
    return AV_PIX_FMT_NONE;
}

enum sxplayer_pixel_format sxpi_pix_fmts_ff2sx(enum AVPixelFormat pix_fmt)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(pix_fmts_mapping); i++)
        if (pix_fmts_mapping[i].ff == pix_fmt)
            return pix_fmts_mapping[i].sx;
    return -1;
}

enum sxplayer_pixel_format sxpi_smp_fmts_ff2sx(enum AVSampleFormat smp_fmt)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(smp_fmts_mapping); i++)
        if (smp_fmts_mapping[i].ff == smp_fmt)
            return smp_fmts_mapping[i].sx;
    return -1;
}

void sxpi_set_thread_name(const char *name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__) && defined(__GLIBC__)
    pthread_setname_np(pthread_self(), name);
#endif
}

void sxpi_update_dimensions(int *width, int *height, int max_pixels)
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
