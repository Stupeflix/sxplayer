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

#ifndef FILTERING_H
#define FILTERING_H

#include <libavutil/threadmessage.h>

struct filtering_ctx *filtering_alloc(void);

int filtering_init(struct filtering_ctx *f,
                   AVThreadMessageQueue *in_queue,
                   AVThreadMessageQueue *out_queue,
                   int sw_pix_fmt,
                   const AVCodecContext *actx);

void filtering_run(struct filtering_ctx *f);

void filtering_uninit(struct filtering_ctx *f);
void filtering_free(struct filtering_ctx **fp);

#endif /* FILTERING_H */
