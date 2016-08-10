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

#include <pthread.h>

#include "bufcount.h"
#include "internal.h"

#define BUFCOUNT_DEBUG 0
#define MIN_BUF 3

struct bufcount_context {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int refcount;   // nb frames + 1 (+1 because context owns a ref)
    int refmax;     // current maximum number of frames in the air
};

int bufcount_create(struct bufcount_context **bufcount, int bufmax)
{
    int ret;
    struct bufcount_context *b;

    *bufcount = NULL;
    if (bufmax < MIN_BUF)
        return AVERROR(EINVAL);
    b = av_mallocz(sizeof(*b));
    if (!b)
        return AVERROR(ENOMEM);
    b->refcount =          1;
    b->refmax   = bufmax + 1;
    ret = pthread_mutex_init(&b->lock, NULL);
    if (ret < 0) {
        av_freep(&b);
        return AVERROR(ret);
    }
    ret = pthread_cond_init(&b->cond, NULL);
    if (ret < 0) {
        pthread_mutex_destroy(&b->lock);
        av_freep(&b);
        return AVERROR(ret);
    }
    *bufcount = b;
    return 0;
}

int bufcount_update_max(struct bufcount_context *b, int n)
{
    if (!b)
        return 0;

    pthread_mutex_lock(&b->lock);
    if (b->refmax - 1 + n < MIN_BUF) {
        pthread_mutex_unlock(&b->lock);
        return AVERROR(EINVAL);
    }
    b->refmax += n;
#if BUFCOUNT_DEBUG
    fprintf(stderr, "[%p] op:[MAX%s%d] frames:%d/%d\n",
            b, n > 0 ? "+" : "", n, b->refcount - 1, b->refmax - 1);
#endif
    pthread_cond_signal(&b->cond);
    pthread_mutex_unlock(&b->lock);
    return 0;
}

void bufcount_update_ref(struct bufcount_context *b, int n)
{
    if (!b || !n)
        return;

    pthread_mutex_lock(&b->lock);

    b->refcount += n;
#if BUFCOUNT_DEBUG
    fprintf(stderr, "[%p] op:[REF%s%d] frames:%d/%d\n",
            b, n > 0 ? "+" : "", n, b->refcount - 1, b->refmax - 1);
#endif

    if (n > 0) {
        // If we have the maximum number of frames flying around, we wait
        while (b->refcount >= b->refmax)
            pthread_cond_wait(&b->cond, &b->lock);
    }

    if (!b->refcount) {
        pthread_mutex_unlock(&b->lock);
        pthread_mutex_destroy(&b->lock);
        av_free(b);
        return;
    }
    pthread_cond_signal(&b->cond);
    pthread_mutex_unlock(&b->lock);
}
