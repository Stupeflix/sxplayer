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

#ifndef MSG_H
#define MSG_H

enum msg_type {
    MSG_FRAME,
    MSG_PACKET,
    MSG_SEEK,
    MSG_INFO,
    MSG_START,
    MSG_STOP,
    MSG_SYNC,
    NB_MSG
};

struct message {
    void *data;
    enum msg_type type;
};

void sxpi_msg_free_data(void *arg);

#endif
