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

#include <stdarg.h>
#include <libavutil/time.h>
#include "internal.h"

void do_log(const char *mod, const char *fmt, ...)
{
    char logline[512];
    va_list arg_list;

    va_start(arg_list, fmt);
    vsnprintf(logline, sizeof(logline), fmt, arg_list);
    va_end(arg_list);

    printf("[sxplayer %f %s] %s", av_gettime() / 1000000., mod, logline);
}
