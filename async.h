/*
 * This file is part of sfxmp.
 *
 * Copyright (c) 2015 Stupeflix
 *
 * sfxmp is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * sfxmp is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with sfxmp; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef ASYNC_H
#define ASYNC_H

#include <pthread.h>
#include <libavcodec/avcodec.h>
#include <libavutil/threadmessage.h>

#include "internal.h"

typedef int (*pull_packet_func_type)(void *priv, AVPacket *pkt);
typedef int (*seek_func_type)(void *priv, int64_t ts);
typedef int (*push_frame_func_type)(void *priv, AVFrame *frame);

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
                           push_frame_func_type push_frame_cb,
                           struct async_decoder **d,
                           AVRational st_timebase);

int async_start(struct async_context *actx);

int async_wait(struct async_context *actx);

void async_free(struct async_context **actxp);

int async_queue_frame(struct async_decoder *d, AVFrame *frame);

#endif /* ASYNC_H */
