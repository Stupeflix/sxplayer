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

#include <pthread.h>
#include <stdarg.h>
#include <libavutil/time.h>
#include "internal.h"

struct log_ctx {
    void *avlog;
    int64_t last_time;
    pthread_mutex_t lock;
    void *user_arg;
    void (*callback)(void *arg, int level, const char *fmt, va_list vl);
};

void log_set_callback(struct log_ctx *ctx, void *arg,
                      void (*callback)(void *arg, int level, const char *fmt, va_list vl))
{
    ctx->user_arg = arg;
    ctx->callback = callback;
}

struct log_ctx *log_alloc(void)
{
    struct log_ctx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    return ctx;
}

int log_init(struct log_ctx *ctx, void *avlog)
{
    ctx->avlog = avlog;
    return AVERROR(pthread_mutex_init(&ctx->lock, NULL));
}

void log_free(struct log_ctx **ctxp)
{
    struct log_ctx *ctx = *ctxp;
    if (!ctx)
        return;
    pthread_mutex_destroy(&ctx->lock);
    av_freep(ctxp);
}

void log_print(void *log_ctx, int log_level, const char *fn, const char *fmt, ...)
{
    struct log_ctx *ctx = log_ctx;
    va_list arg_list;

    if (ctx->callback) {
        va_start(arg_list, fmt);
        pthread_mutex_lock(&ctx->lock);
        ctx->callback(ctx->user_arg, log_level, fmt, arg_list);
        pthread_mutex_unlock(&ctx->lock);
        va_end(arg_list);
    } else {
        char logline[512];
        static const int av_log_levels[] = {
            [SXPLAYER_LOG_VERBOSE] = AV_LOG_VERBOSE,
            [SXPLAYER_LOG_DEBUG]   = AV_LOG_DEBUG,
            [SXPLAYER_LOG_INFO]    = AV_LOG_INFO,
            [SXPLAYER_LOG_WARNING] = AV_LOG_WARNING,
            [SXPLAYER_LOG_ERROR]   = AV_LOG_ERROR,
        };
        const int av_log_level = av_log_levels[log_level];

        va_start(arg_list, fmt);
        vsnprintf(logline, sizeof(logline), fmt, arg_list);
        va_end(arg_list);

        if (ENABLE_DBG) {
            int64_t t;
            pthread_mutex_lock(&ctx->lock);
            t = av_gettime();
            if (!ctx->last_time)
                ctx->last_time = t;
            av_log(ctx->avlog, av_log_level, "[%f] %s: %s\n", (t - ctx->last_time) / 1000000., fn, logline);
            ctx->last_time = t;
            pthread_mutex_unlock(&ctx->lock);
        } else {
            av_log(ctx->avlog, av_log_level, "%s: %s\n", fn, logline);
        }
    }
}
