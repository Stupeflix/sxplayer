#include <libavcodec/avcodec.h>
#include <libavcodec/videotoolbox.h>

#include "internal.h"

#if 1
#define REQUESTED_PIX_FMT kCVPixelFormatType_32BGRA
#elif 0
#define REQUESTED_PIX_FMT kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
#else
#define REQUESTED_PIX_FMT kCVPixelFormatType_32RGBA
#endif

static void vt_uninit(AVCodecContext *avctx)
{
    av_videotoolbox_default_free(avctx);
}

static int vt_get_frame(AVCodecContext *avctx, AVFrame *frame)
{
    if (ENABLE_DBG) {
        char tag[32];
        CVPixelBufferRef pixbuf = (CVPixelBufferRef)frame->data[3];
        OSType pixel_format = CVPixelBufferGetPixelFormatType(pixbuf);
        av_get_codec_tag_string(tag, sizeof(tag), pixel_format);
        DBG("VT", "get frame pix fmt: %s / pixbuf=%p\n", tag, pixbuf);
    }
    return 0;
}

static int vt_decoder_init(AVCodecContext *avctx)
{
    int ret;
    struct hwaccel_ctx *s = avctx->opaque;

    av_videotoolbox_default_free(avctx);

    AVVideotoolboxContext *vtctx = av_videotoolbox_alloc_context();
    vtctx->cv_pix_fmt_type = REQUESTED_PIX_FMT;
    s->internal_ctx = vtctx;
    ret = av_videotoolbox_default_init2(avctx, vtctx);
    if (ret < 0)
        goto err;

    return 0;

err:
    vt_uninit(avctx);
    return ret;
}

const struct hwaccel hwaccel_vt = {
    .decoder_init   = vt_decoder_init,
    .uninit         = vt_uninit,
    .get_frame      = vt_get_frame,
    .pix_fmt        = AV_PIX_FMT_VIDEOTOOLBOX,
};
