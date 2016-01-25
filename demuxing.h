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

#ifndef DEMUXING_H
#define DEMUXING_H

#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavutil/threadmessage.h>

struct demuxing_ctx *demuxing_alloc(void);

int demuxing_init(void *log_ctx,
                  struct demuxing_ctx *ctx,
                  AVThreadMessageQueue *pkt_queue,
                  const char *filename, int avselect,
                  int pkt_skip_mod);

int demuxing_seek(struct demuxing_ctx *ctx, int64_t ts);

int64_t demuxing_probe_duration(const struct demuxing_ctx *ctx);
double demuxing_probe_rotation(const struct demuxing_ctx *ctx);
const AVStream *demuxing_get_stream(const struct demuxing_ctx *ctx);

void demuxing_run(struct demuxing_ctx *ctx);

void demuxing_free(struct demuxing_ctx **ctxp);

#endif /* DEMUXING_H */
