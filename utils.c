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
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__) && defined(__GLIBC__)
    pthread_setname_np(pthread_self(), name);
#endif
}
