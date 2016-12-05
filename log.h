/*
 * This file is part of sxplayer.
 *
 * Copyright (c) 2016 Stupeflix
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

#ifndef LOG_H
#define LOG_H

#include <libavutil/log.h>

#include "sxplayer.h"

/* ENABLE_DBG can be set with the build system using TRACE=yes option. It will
 * enable the compilation of the tracing logging */
#ifndef ENABLE_DBG
# define ENABLE_DBG 0
#endif

#if ENABLE_DBG
# define LOG_LEVEL AV_LOG_DEBUG
#else
/* The following will affect the default usage (aka no user logging callback specified) */
# define LOG_LEVEL AV_LOG_ERROR  // will log only errors
//# define LOG_LEVEL AV_LOG_WARNING // will log errors, warnings
//# define LOG_LEVEL AV_LOG_INFO   // will log little information such as file opening and decoder in use
//# define LOG_LEVEL AV_LOG_DEBUG  // will log most of the important actions (get/ret frame)
#endif

#define DO_LOG(c, log_level, ...) sxpi_log_print((c)->log_ctx, log_level, \
                                                 __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define LOG(c, level, ...) DO_LOG(c, SXPLAYER_LOG_##level, __VA_ARGS__)

#if ENABLE_DBG
#define TRACE(c, ...) do { DO_LOG(c, SXPLAYER_LOG_VERBOSE, __VA_ARGS__); fflush(stdout); } while (0)
#else
/* Note: this could be replaced by a "while(0)" but it wouldn't test the
 * compilation of the printf format, so we use this more complex form. */
#define TRACE(c, ...) do { if (0) DO_LOG(c, SXPLAYER_LOG_VERBOSE, __VA_ARGS__); } while (0)
#endif

struct log_ctx;

struct log_ctx *sxpi_log_alloc(void);

int sxpi_log_init(struct log_ctx *ctx, void *avlog);

void sxpi_log_set_callback(struct log_ctx *ctx, void *arg,
                           sxplayer_log_callback_type callback);

void sxpi_log_print(void *log_ctx, int log_level, const char *filename,
                    int ln, const char *fn, const char *fmt, ...) av_printf_format(6, 7);

void sxpi_log_free(struct log_ctx **ctxp);

#endif /* LOG_H */
