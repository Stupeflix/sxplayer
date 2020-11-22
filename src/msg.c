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

#include <libavutil/frame.h>
#include <libavutil/avassert.h>
#include <libavcodec/avcodec.h>

#include "msg.h"

void sxpi_msg_free_data(void *arg)
{
    struct message *msg = arg;

    switch (msg->type) {
    case MSG_FRAME: {
        AVFrame *frame = msg->data;
        av_frame_free(&frame);
        msg->data = NULL;
        break;
    }
    case MSG_PACKET:
        av_packet_unref(msg->data);
        av_freep(&msg->data);
        break;
    case MSG_SEEK:
    case MSG_INFO:
        av_freep(&msg->data);
        break;
    case MSG_START:
    case MSG_STOP:
    case MSG_SYNC:
        break;
    default:
        av_assert0(0);
    }
}
