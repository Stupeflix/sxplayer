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

#include "sxplayer.h"
#include "opts.h"
#include "msg.h"

const char *sxpi_async_get_msg_type_string(enum msg_type type);

void sxpi_msg_free_data(void *arg);

struct async_context *sxpi_async_alloc_context(void);

int sxpi_async_init(struct async_context *actx, void *log_ctx,
                    const char *filename, const struct sxplayer_opts *o);

int sxpi_async_start(struct async_context *actx);

int sxpi_async_fetch_info(struct async_context *actx, struct sxplayer_info *info);

int sxpi_async_seek(struct async_context *actx, int64_t ts);

int sxpi_async_pop_frame(struct async_context *actx, AVFrame **framep);

int sxpi_async_stop(struct async_context *actx);

int sxpi_sxpi_async_started(struct async_context *actx);

void sxpi_async_free(struct async_context **actxp);

#endif /* ASYNC_H */
