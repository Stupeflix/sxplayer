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

#ifndef MOD_DEMUXING_H
#define MOD_DEMUXING_H

#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavutil/threadmessage.h>

#include "opts.h"

struct demuxing_ctx *sxpi_demuxing_alloc(void);

int sxpi_demuxing_init(void *log_ctx,
                       struct demuxing_ctx *ctx,
                       AVThreadMessageQueue *src_queue,
                       AVThreadMessageQueue *pkt_queue,
                       const char *filename,
                       const struct sxplayer_opts *opts);

int64_t sxpi_demuxing_probe_duration(const struct demuxing_ctx *ctx);
double sxpi_demuxing_probe_rotation(const struct demuxing_ctx *ctx);
const AVStream *sxpi_demuxing_get_stream(const struct demuxing_ctx *ctx);
int sxpi_demuxing_is_image(const struct demuxing_ctx *ctx);

void sxpi_demuxing_run(struct demuxing_ctx *ctx);

void sxpi_demuxing_free(struct demuxing_ctx **ctxp);

#endif
