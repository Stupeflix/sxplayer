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

#ifndef ASYNC_H
#define ASYNC_H

#include <stdint.h>
#include <libavutil/rational.h>

#include "sxplayer.h"
#include "internal.h"

struct async_context *async_alloc_context(void);

int async_init(struct async_context *actx, const struct sxplayer_ctx *s);

int async_start(struct async_context *actx);

int64_t async_probe_duration(const struct async_context *actx);

int async_fetch_info(const struct async_context *actx, struct sxplayer_info *info);

int async_seek(struct async_context *actx, int64_t ts);

AVFrame *async_pop_frame(struct async_context *actx);

int async_wait(struct async_context *actx);

int async_stop(struct async_context *actx);

int async_started(const struct async_context *actx);

void async_free(struct async_context **actxp);

#endif /* ASYNC_H */
