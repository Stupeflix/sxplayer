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

#ifndef MOD_FILTERING_H
#define MOD_FILTERING_H

#include <libavcodec/avcodec.h>
#include <libavutil/threadmessage.h>

#include "opts.h"

struct filtering_ctx *sxpi_filtering_alloc(void);

int sxpi_filtering_init(void *log_ctx,
                        struct filtering_ctx *ctx,
                        AVThreadMessageQueue *in_queue,
                        AVThreadMessageQueue *out_queue,
                        const AVStream *stream,
                        const AVCodecContext *avctx,
                        double media_rotation,
                        const struct sxplayer_opts *o);

void sxpi_filtering_run(struct filtering_ctx *ctx);

void sxpi_filtering_free(struct filtering_ctx **ctxp);

#endif
