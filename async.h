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

#include <pthread.h>
#include <libavcodec/avcodec.h>

#include "internal.h"
#include "filtering.h"

typedef int (*pull_packet_func_type)(void *priv, AVPacket *pkt);
typedef int (*seek_func_type)(void *priv, int64_t ts);

struct async_decoder;
struct async_reader;
struct async_context;

struct decoder_ctx;

struct async_context *async_alloc_context(void);

int async_register_reader(struct async_context *actx,
                          void *priv,
                          pull_packet_func_type pull_packet_cb,
                          seek_func_type seek_cb,
                          struct async_reader **r);

int async_reader_seek(struct async_reader *r, int64_t ts);

int async_register_decoder(struct async_reader *r,
                           struct decoder_ctx *codec_ctx, void *priv,
                           struct async_decoder **d,
                           AVRational st_timebase,
                           int sw_pix_fmt);

int async_register_filterer(struct async_decoder *d,
                            const char *filters,
                            int64_t trim_duration);

int async_start(struct async_context *actx, int64_t skip);

int async_wait(struct async_context *actx);

int async_stop(struct async_context *actx);

int async_started(struct async_context *actx);

void async_free(struct async_context **actxp);

AVFrame *async_pop_frame(struct async_context *actx);

int async_queue_frame(struct async_decoder *d, AVFrame *frame);

#endif /* ASYNC_H */
