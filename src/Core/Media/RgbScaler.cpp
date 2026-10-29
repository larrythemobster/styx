// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#include "Core/Media/RgbScaler.hpp"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace Styx::Core::Media {

namespace {

constexpr int HD_HEIGHT = 720;
constexpr int SWSCALE_OVERRUN_BYTES = 64;

bool isYuv(int format) {
    const AVPixFmtDescriptor *layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
    return layout && !(layout->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL));
}

} // namespace

bool usesBt709(const AVFrame *frame) {
    if (frame->colorspace == AVCOL_SPC_BT709)
        return true;
    if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
        return false;
    return frame->height >= HD_HEIGHT;
}

bool isFullRange(const AVFrame *frame) {
    if (frame->color_range == AVCOL_RANGE_JPEG)
        return true;

    // The formats ffmpeg keeps for full-range pictures from decoders that do not set the range.
    switch (frame->format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUVJ440P:
    case AV_PIX_FMT_YUVJ411P:
        return true;
    default:
        return false;
    }
}

RgbScaler::~RgbScaler() {
    sws_freeContext(context);
}

Platform::ImageBuffer RgbScaler::allocate(const Video::FrameSize &size) {
    if (size.width <= 0 || size.height <= 0)
        return {};

    Platform::ImageBuffer image;
    image.width = size.width;
    image.height = size.height;
    image.channels = 4;
    // Overrun bytes to accommodate swscale SIMD reads/writes beyond end of buffer
    image.pixels.resize(static_cast<size_t>(size.width) * size.height * 4 + SWSCALE_OVERRUN_BYTES);
    return image;
}

bool RgbScaler::scale(const AVFrame *frame, Platform::ImageBuffer &into) {
    if (into.width <= 0 || into.height <= 0)
        return false;

    if (into.pixels.size() < static_cast<size_t>(into.width) * into.height * 4 + SWSCALE_OVERRUN_BYTES) {
        into = allocate({into.width, into.height});
    }

    const Source source{frame->width, frame->height, frame->format, usesBt709(frame), isFullRange(frame), {into.width, into.height}};
    context = sws_getCachedContext(context, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), into.width, into.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!context)
        return false;

    if (source != configuredFor) {
        configuredFor = source;
        if (isYuv(frame->format)) {
            const int *coefficients = sws_getCoefficients(source.bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
            sws_setColorspaceDetails(context, coefficients, source.fullRange ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        }
    }

    uint8_t *planes[4] = {into.pixels.data(), nullptr, nullptr, nullptr};
    const int strides[4] = {into.width * 4, 0, 0, 0};
    return sws_scale(context, frame->data, frame->linesize, 0, frame->height, planes, strides) > 0;
}

} // namespace Styx::Core::Media
