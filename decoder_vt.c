/*
 * This file is part of sxplayer.
 *
 * Copyright (c) 2015 Stupeflix
 * Copyright (c) 2012 Sebastien Zwickert
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

#define Picture QuickdrawPicture
#include <VideoToolbox/VideoToolbox.h>
#undef Picture

#include "decoding.h"
#include "internal.h"

#define REQUESTED_PIX_FMT kCVPixelFormatType_32BGRA

struct async_frame {
    int64_t pts;
    CVPixelBufferRef cv_buffer;
    struct async_frame *next_frame;
};

struct vtdec_context {
    VTDecompressionSessionRef session;
    VTDecompressionOutputCallback output_callback;
    OSType cv_pix_fmt_type;
    CMVideoFormatDescriptionRef cm_fmt_desc;
    struct async_frame *queue;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int nb_frames;
    int terminated;
};

static CMVideoFormatDescriptionRef videotoolbox_format_desc_create(CMVideoCodecType codec_type,
                                                                   CFDictionaryRef decoder_spec,
                                                                   int width,
                                                                   int height)
{
    CMFormatDescriptionRef cm_fmt_desc;
    OSStatus status;

    status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault,
                                            codec_type,
                                            width,
                                            height,
                                            decoder_spec, // Dictionary of extension
                                            &cm_fmt_desc);

    if (status)
        return NULL;

    return cm_fmt_desc;
}

static void dict_set_data(CFMutableDictionaryRef dict, CFStringRef key, uint8_t * value, uint64_t length)
{
    CFDataRef data;
    data = CFDataCreate(NULL, value, (CFIndex)length);
    CFDictionarySetValue(dict, key, data);
    CFRelease(data);
}

static CFDictionaryRef videotoolbox_decoder_config_create(CMVideoCodecType codec_type,
                                                          const AVCodecContext *avctx)
{
    CFMutableDictionaryRef config_info = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                                   1,
                                                                   &kCFTypeDictionaryKeyCallBacks,
                                                                   &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(config_info,
                         CFSTR("EnableHardwareAcceleratedVideoDecoder"),
                         kCFBooleanTrue);

    if (avctx->extradata_size) {
        CFMutableDictionaryRef avc_info;

        avc_info = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                             1,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);

        dict_set_data(avc_info, CFSTR("avcC"), avctx->extradata, avctx->extradata_size);

        CFDictionarySetValue(config_info,
                kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms,
                avc_info);

        CFRelease(avc_info);
    }
    return config_info;
}

static CFDictionaryRef videotoolbox_buffer_attributes_create(int width,
                                                             int height,
                                                             OSType pix_fmt)
{
    CFMutableDictionaryRef buffer_attributes;
    CFMutableDictionaryRef io_surface_properties;
    CFNumberRef cv_pix_fmt;
    CFNumberRef w;
    CFNumberRef h;

    w = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &width);
    h = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &height);
    cv_pix_fmt = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pix_fmt);

    buffer_attributes = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                  4,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    io_surface_properties = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                      0,
                                                      &kCFTypeDictionaryKeyCallBacks,
                                                      &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(buffer_attributes, kCVPixelBufferPixelFormatTypeKey, cv_pix_fmt);
    CFDictionarySetValue(buffer_attributes, kCVPixelBufferIOSurfacePropertiesKey, io_surface_properties);
    CFDictionarySetValue(buffer_attributes, kCVPixelBufferWidthKey, w);
    CFDictionarySetValue(buffer_attributes, kCVPixelBufferHeightKey, h);

    CFRelease(io_surface_properties);
    CFRelease(cv_pix_fmt);
    CFRelease(w);
    CFRelease(h);

    return buffer_attributes;
}

static void videotoolbox_buffer_release(void *opaque, uint8_t *data)
{
    CVPixelBufferRef cv_buffer = (CVImageBufferRef)data;
    CVPixelBufferRelease(cv_buffer);
}

static int push_async_frame(const AVCodecContext *avctx,
                            struct decoder_ctx *dec_ctx,
                            struct async_frame *async_frame)
{
    int ret;
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    frame->width   = avctx->width;
    frame->height  = avctx->height;
    frame->format  = avctx->pix_fmt;
    frame->pts     = async_frame->pts;
    frame->data[3] = (uint8_t *)async_frame->cv_buffer;
    frame->buf[0]  = av_buffer_create(frame->data[3],
                                      sizeof(frame->data[3]),
                                      videotoolbox_buffer_release,
                                      NULL,
                                      AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }
    TRACE(dec_ctx, "push frame pts=%"PRId64" to AsyncDecoder %p",
          frame->pts, dec_ctx->adec);
    ret = async_queue_frame(dec_ctx->adec, frame);
    if (ret < 0)
        av_frame_free(&frame);
    return ret;
}

static void decode_callback(void *opaque,
                            void *sourceFrameRefCon,
                            OSStatus status,
                            VTDecodeInfoFlags flags,
                            CVImageBufferRef image_buffer,
                            CMTime pts,
                            CMTime duration)
{
    struct decoder_ctx *dec_ctx = opaque;
    const AVCodecContext *avctx = dec_ctx->avctx;
    struct vtdec_context *vt = dec_ctx->priv_data;
    struct async_frame *new_frame;
    struct async_frame *queue_walker;

    TRACE(dec_ctx, "entering decode callback");

    if (!image_buffer) {
        TRACE(dec_ctx, "decode cb received NULL output image buffer");
        return;
    }

    new_frame = av_mallocz(sizeof(struct async_frame));
    new_frame->next_frame = NULL;
    new_frame->cv_buffer = CVPixelBufferRetain(image_buffer);
    new_frame->pts = pts.value;

    pthread_mutex_lock(&vt->lock);

    while (vt->nb_frames == 6 && !vt->terminated)
        pthread_cond_wait(&vt->cond, &vt->lock);

    queue_walker = vt->queue;

    if (!queue_walker || (new_frame->pts < queue_walker->pts)) {
        /* we have an empty queue, or this frame earlier than the current queue head */
        new_frame->next_frame = queue_walker;
        vt->queue = new_frame;
        TRACE(dec_ctx, "queueing frame pts=%"PRId64" at pos=%d",
              new_frame->pts, vt->nb_frames);
        vt->nb_frames++;
    } else {
        /* walk the queue and insert this frame where it belongs in display order */
        struct async_frame *next_frame;

        while (1) {
            next_frame = queue_walker->next_frame;

            if (!next_frame || (new_frame->pts < next_frame->pts)) {
                new_frame->next_frame = next_frame;
                queue_walker->next_frame = new_frame;
                TRACE(dec_ctx, "queueing frame pts=%"PRId64" at pos=%d",
                      new_frame->pts, vt->nb_frames);
                vt->nb_frames++;
                break;
            }

            /* We passed a frame, which as a result becomes a valid frame to push */
            push_async_frame(avctx, dec_ctx, queue_walker);
            av_free(queue_walker);
            vt->nb_frames--;
            vt->queue = queue_walker = next_frame;
        }
    }

    pthread_mutex_unlock(&vt->lock);
}

static int vtdec_init(struct decoder_ctx *dec_ctx)
{
    const AVCodecContext *avctx = dec_ctx->avctx;
    struct vtdec_context *vt = dec_ctx->priv_data;
    int cm_codec_type;
    OSStatus status;
    VTDecompressionOutputCallbackRecord decoder_cb;
    CFDictionaryRef decoder_spec;
    CFDictionaryRef buf_attr;

    TRACE(dec_ctx, "init");

    avctx->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;

    pthread_mutex_init(&vt->lock, NULL);
    pthread_cond_init(&vt->cond, NULL);

    switch (avctx->codec_id) {
    case AV_CODEC_ID_H263:       cm_codec_type = kCMVideoCodecType_H263;       break;
    case AV_CODEC_ID_H264:       cm_codec_type = kCMVideoCodecType_H264;       break;
    case AV_CODEC_ID_MPEG1VIDEO: cm_codec_type = kCMVideoCodecType_MPEG1Video; break;
    case AV_CODEC_ID_MPEG2VIDEO: cm_codec_type = kCMVideoCodecType_MPEG2Video; break;
    case AV_CODEC_ID_MPEG4:      cm_codec_type = kCMVideoCodecType_MPEG4Video; break;
    default:
        return AVERROR_DECODER_NOT_FOUND;
    }

    decoder_spec = videotoolbox_decoder_config_create(cm_codec_type, avctx);

    vt->cm_fmt_desc = videotoolbox_format_desc_create(cm_codec_type,
                                                       decoder_spec,
                                                       avctx->width,
                                                       avctx->height);
    if (!vt->cm_fmt_desc) {
        if (decoder_spec)
            CFRelease(decoder_spec);

        fprintf(stderr, "format description creation failed\n");
        return AVERROR_EXTERNAL;
    }

    buf_attr = videotoolbox_buffer_attributes_create(avctx->width,
                                                     avctx->height,
                                                     REQUESTED_PIX_FMT);

    decoder_cb.decompressionOutputCallback = decode_callback;
    decoder_cb.decompressionOutputRefCon   = dec_ctx;

    status = VTDecompressionSessionCreate(NULL,
                                          vt->cm_fmt_desc,
                                          decoder_spec,
                                          buf_attr,
                                          &decoder_cb,
                                          &vt->session);

    if (decoder_spec)
        CFRelease(decoder_spec);
    if (buf_attr)
        CFRelease(buf_attr);

    switch (status) {
    case kVTVideoDecoderNotAvailableNowErr:
    case kVTVideoDecoderUnsupportedDataFormatErr:
        return AVERROR(ENOSYS);
    case kVTVideoDecoderMalfunctionErr:
        return AVERROR(EINVAL);
    case kVTVideoDecoderBadDataErr :
        return AVERROR_INVALIDDATA;
    case 0:
        return 0;
    default:
        return AVERROR_UNKNOWN;
    }
}

static CMSampleBufferRef videotoolbox_sample_buffer_create(CMFormatDescriptionRef fmt_desc,
                                                           void *buffer,
                                                           int size,
                                                           int64_t frame_pts)
{
    OSStatus status;
    CMBlockBufferRef  block_buf;
    CMSampleBufferRef sample_buf;
    CMSampleTimingInfo timeInfo;
    CMSampleTimingInfo timeInfoArray[1];

    timeInfo.presentationTimeStamp = CMTimeMake(frame_pts, 1);
    timeInfoArray[0] = timeInfo;

    block_buf  = NULL;
    sample_buf = NULL;

    status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,// structureAllocator
                                                buffer,             // memoryBlock
                                                size,               // blockLength
                                                kCFAllocatorNull,   // blockAllocator
                                                NULL,               // customBlockSource
                                                0,                  // offsetToData
                                                size,               // dataLength
                                                0,                  // flags
                                                &block_buf);

    if (!status) {
        status = CMSampleBufferCreate(kCFAllocatorDefault,  // allocator
                                      block_buf,            // dataBuffer
                                      TRUE,                 // dataReady
                                      0,                    // makeDataReadyCallback
                                      0,                    // makeDataReadyRefcon
                                      fmt_desc,             // formatDescription
                                      1,                    // numSamples
                                      1,                    // numSampleTimingEntries
                                      timeInfoArray,        // sampleTimingArray
                                      0,                    // numSampleSizeEntries
                                      NULL,                 // sampleSizeArray
                                      &sample_buf);
    }

    if (block_buf)
        CFRelease(block_buf);

    return sample_buf;
}

static int vtdec_push_packet(struct decoder_ctx *dec_ctx, const AVPacket *pkt)
{
    int status;
    struct vtdec_context *vt = dec_ctx->priv_data;

    if (!pkt->size) {
        VTDecompressionSessionFinishDelayedFrames(vt->session);
        return AVERROR_EOF;
    }

    VTDecodeFrameFlags decodeFlags = kVTDecodeFrame_EnableAsynchronousDecompression;
    CMSampleBufferRef sample_buf = videotoolbox_sample_buffer_create(vt->cm_fmt_desc, pkt->data, pkt->size, pkt->pts);

    if (!sample_buf)
        return AVERROR_EXTERNAL;

    status = VTDecompressionSessionDecodeFrame(vt->session,
                                               sample_buf,
                                               decodeFlags,
                                               NULL,    // sourceFrameRefCon
                                               0);      // infoFlagsOut

#if 0
    if (status == noErr)
        status = VTDecompressionSessionWaitForAsynchronousFrames(vt->session);
#endif

    CFRelease(sample_buf);

    if (status) {
        av_log(NULL, AV_LOG_ERROR, "Failed to decode frame (%d)\n", status);
        return AVERROR_EXTERNAL;
    }

    return pkt->size;
}

static void vtdec_flush(struct decoder_ctx *dec_ctx)
{
    struct vtdec_context *vt = dec_ctx->priv_data;

    TRACE(dec_ctx, "flushing");
    if (vt->session) {
        VTDecompressionSessionFinishDelayedFrames(vt->session);
        VTDecompressionSessionWaitForAsynchronousFrames(vt->session);
    }
    TRACE(dec_ctx, "decompression session finished delaying frames");
    async_queue_frame(dec_ctx->adec, NULL);
    pthread_mutex_lock(&vt->lock);
    TRACE(dec_ctx, "dropping %d frames", vt->nb_frames);
    while (vt->queue != NULL) {
        struct async_frame *top_frame = vt->queue;
        vt->queue = top_frame->next_frame;
        av_freep(&top_frame);
    }
    vt->nb_frames = 0;
    pthread_cond_signal(&vt->cond);
    pthread_mutex_unlock(&vt->lock);
    TRACE(dec_ctx, "queue cleared, flush ends");
}

static void vtdec_uninit(struct decoder_ctx *dec_ctx)
{
    struct vtdec_context *vt = dec_ctx->priv_data;

    TRACE(dec_ctx, "uninit");
    if (vt->cm_fmt_desc)
        CFRelease(vt->cm_fmt_desc);

    pthread_mutex_lock(&vt->lock);
    vt->terminated = 1;
    pthread_cond_signal(&vt->cond);
    pthread_mutex_unlock(&vt->lock);

    vtdec_flush(dec_ctx);

    if (vt->session) {
        VTDecompressionSessionInvalidate(vt->session);
        vt->session = NULL;
    }

    pthread_mutex_destroy(&vt->lock);
}

const struct decoder decoder_vt = {
    .name             = "videotoolbox",
    .init             = vtdec_init,
    .push_packet      = vtdec_push_packet,
    .flush            = vtdec_flush,
    .uninit           = vtdec_uninit,
    .priv_data_size   = sizeof(struct vtdec_context),
};
