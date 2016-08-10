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

#ifndef BUFCOUNT_H
#define BUFCOUNT_H

struct bufcount_context;

/**
 * Allocate and initialize a buffer counter context
 *
 * @param bufmax maximum number of simultaneous buffers
 *
 * @return negative value on error, 0 otherwise
 */
int bufcount_create(struct bufcount_context **bufcount, int bufmax);

/**
 * Adjust the maximum number of simultaneous buffers
 *
 * @param n number of buffer to add to the current maximum, can be negative
 *
 * @return negative value on error, 0 otherwise
 */
int bufcount_update_max(struct bufcount_context *b, int n);

/**
 * Update current number of buffers. If n is positive, this function may block
 * until the number of buffer is reduced under the current maximum number of
 * buffer.
 */
void bufcount_update_ref(struct bufcount_context *b, int n);

#endif
