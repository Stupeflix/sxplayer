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

#ifndef DECODING_H
#define DECODING_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/threadmessage.h>

struct decoding_ctx *decoding_alloc(void);

int decoding_init(void *log_ctx,
                  struct decoding_ctx *ctx,
                  AVThreadMessageQueue *pkt_queue,
                  AVThreadMessageQueue *frames_queue,
                  const AVStream *stream,
                  int auto_hwaccel,
                  int export_mvs,
                  void *opaque,
                  int max_pixels);

const AVCodecContext *decoding_get_avctx(struct decoding_ctx *ctx);

int decoding_queue_frame(struct decoding_ctx *ctx, AVFrame *frame);

void decoding_run(struct decoding_ctx *ctx);

void decoding_free(struct decoding_ctx **ctxp);

#endif /* DECODING_H */
