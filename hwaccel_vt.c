#include <libavcodec/avcodec.h>
#include <libavcodec/videotoolbox.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>

#include "internal.h"

struct vt_ctx {
    AVFrame *tmp_frame;
};

#if 0
#define REQUESTED_PIX_FMT kCVPixelFormatType_32BGRA
#define FFMPEG_PIX_FMT    AV_PIX_FMT_BGRA
#elif 1
#define REQUESTED_PIX_FMT kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
#define FFMPEG_PIX_FMT    AV_PIX_FMT_NV12
#else
#define REQUESTED_PIX_FMT kCVPixelFormatType_32RGBA
#define FFMPEG_PIX_FMT    AV_PIX_FMT_RGBA
#endif

static void vt_uninit(AVCodecContext *avctx)
{
    struct hwaccel_ctx *s = avctx->opaque;
    struct vt_ctx *vt = s->internal_ctx;

    av_frame_free(&vt->tmp_frame);
    av_videotoolbox_default_free(avctx);
    av_freep(&vt);
}

static int vt_decoder_init(AVCodecContext *avctx)
{
    int ret;
    struct hwaccel_ctx *s = avctx->opaque;
    struct vt_ctx *vt = av_mallocz(sizeof(*vt));

    if (!vt)
        return AVERROR(ENOMEM);

    vt->tmp_frame = av_frame_alloc();
    if (!vt->tmp_frame) {
        ret = AVERROR(ENOMEM);
        goto err;
    }

    s->internal_ctx = vt;

    av_videotoolbox_default_free(avctx);

    AVVideotoolboxContext *vtctx = av_videotoolbox_alloc_context();
    vtctx->cv_pix_fmt_type = REQUESTED_PIX_FMT;
    ret = av_videotoolbox_default_init2(avctx, vtctx);
    if (ret < 0)
        goto err;

    return 0;

err:
    vt_uninit(avctx);
    return ret;
}

static int vt_get_frame(AVCodecContext *avctx, AVFrame *frame)
{
    int ret;
    struct hwaccel_ctx *s = avctx->opaque;
    struct vt_ctx *vt = s->internal_ctx;
    CVPixelBufferRef pixbuf = (CVPixelBufferRef)frame->data[3];
    OSType pixel_format = CVPixelBufferGetPixelFormatType(pixbuf);
    CVReturn err;
    uint8_t *data[4] = {0};
    int linesize[4] = {0};

    DBG("VT", "get frame pix fmt: %08x / pixbuf=%p\n", pixel_format, pixbuf);

    av_frame_unref(vt->tmp_frame);

    av_assert0(pixel_format == REQUESTED_PIX_FMT);
    vt->tmp_frame->format = FFMPEG_PIX_FMT;

    // TODO: do not copy
    vt->tmp_frame->width  = frame->width;
    vt->tmp_frame->height = frame->height;
    ret = av_frame_get_buffer(vt->tmp_frame, 32);
    if (ret < 0) {
        fprintf(stderr, "unable to get buffer with tmp frame %p with %dx%d: %s\n", vt->tmp_frame,
                frame->width, frame->height, av_err2str(ret));
        return ret;
    }

    err = CVPixelBufferLockBaseAddress(pixbuf, kCVPixelBufferLock_ReadOnly);
    if (err != kCVReturnSuccess) {
        fprintf(stderr, "unable to lock pixel buffer\n");
        return AVERROR_UNKNOWN;
    }

    if (CVPixelBufferIsPlanar(pixbuf)) {
        int i;
        const int planes = CVPixelBufferGetPlaneCount(pixbuf);

        for (i = 0; i < planes; i++) {
            data[i]     = CVPixelBufferGetBaseAddressOfPlane(pixbuf, i);
            linesize[i] = CVPixelBufferGetBytesPerRowOfPlane(pixbuf, i);
        }
    } else {
        data[0] = CVPixelBufferGetBaseAddress(pixbuf);
        linesize[0] = CVPixelBufferGetBytesPerRow(pixbuf);
    }

    av_image_copy(vt->tmp_frame->data, vt->tmp_frame->linesize,
                  (const uint8_t **)data, linesize, vt->tmp_frame->format,
                  frame->width, frame->height);

    ret = av_frame_copy_props(vt->tmp_frame, frame);
    CVPixelBufferUnlockBaseAddress(pixbuf, kCVPixelBufferLock_ReadOnly);
    if (ret < 0) {
        fprintf(stderr, "copy props fails between %p and %p\n", vt->tmp_frame, frame);
        return ret;
    }

    av_frame_unref(frame);
    av_frame_move_ref(frame, vt->tmp_frame);

    return 0;
}

const struct hwaccel hwaccel_vt = {
    .decoder_init   = vt_decoder_init,
    .uninit         = vt_uninit,
    .get_frame      = vt_get_frame,
    .pix_fmt        = AV_PIX_FMT_VIDEOTOOLBOX,
};
