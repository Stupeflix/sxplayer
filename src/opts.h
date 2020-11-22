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

#ifndef OPTS_H
#define OPTS_H

#include <stdint.h>

struct sxplayer_opts {
    int avselect;                           // select audio or video
    double skip;                            // see public header
    double trim_duration;                   // see public header
    double dist_time_seek_trigger;          // see public header
    int max_nb_frames;                      // maximum number of frames in the queue
    int max_nb_packets;                     // maximum number of packets in the queue
    int max_nb_sink;                        // maximum number of frames in the filtered queue
    char *filters;                          // user filter graph string
    int sw_pix_fmt;                         // sx pixel format to use for software decoding
    int autorotate;                         // switch for automatically rotate in software decoding
    int auto_hwaccel;                       // attempt to enable hardware acceleration
    int export_mvs;                         // export motion vectors into frame->mvs
    int pkt_skip_mod;                       // skip packet if module pkt_skip_mod (and not a key pkt)
    int thread_stack_size;
    void *opaque;                           // pointer to an opaque pointer forwarded to the decoder
    int opaque_size;                        // opaque pointer size
    int max_pixels;                         // maximum number of pixels per frame
    int audio_texture;                      // output audio as a video texture
    char *vt_pix_fmt;                       // VideoToolbox pixel format in the CVPixelBufferRef
    int stream_idx;
    int use_pkt_duration;

    int64_t skip64;
    int64_t trim_duration64;
    int64_t dist_time_seek_trigger64;
};

#endif
