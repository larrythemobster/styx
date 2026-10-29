#include "StreamDecoder.hpp"

#ifdef STYX_HAVE_FFMPEG

#include "Core/Logging.hpp"
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace Styx {
namespace Core {
namespace Stream {

namespace {

AVPixelFormat pickHardwareFormat(AVCodecContext *, const AVPixelFormat *formats)
{
    for (const AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_D3D11)
            return *f;
    }
    // no hardware surface on offer: take the software format the decoder lists first
    for (const AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (!(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *f;
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

StreamDecoder::~StreamDecoder()
{
    close();
}

void StreamDecoder::setTargetSize(const Platform::Size &size)
{
    if (size.empty())
        return;
    targetWidth = size.width;
    targetHeight = size.height;
}

void StreamDecoder::close()
{
    avcodec_free_context(&ctx);
    av_buffer_unref(&hwDevice);
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&swFrame);
    sws_freeContext(sws);
    sws = nullptr;
}

bool StreamDecoder::openDecoder(bool useHardware)
{
    close();
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
        return false;
    ctx = avcodec_alloc_context3(codec);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    hardware = false;
    if (useHardware) {
#ifdef _WIN32
        if (av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
            ctx->hw_device_ctx = av_buffer_ref(hwDevice);
            ctx->get_format = pickHardwareFormat;
            hardware = true;
        }
#endif
    }
    if (!hardware)
        ctx->thread_count = 0; // slice threads keep latency at one frame

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        close();
        return false;
    }
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    swFrame = av_frame_alloc();
    log(LogLevel::Info, LogStream) << "Stream decoder opened," << (hardware ? "D3D11VA" : "software");
    return true;
}

void StreamDecoder::decode(const std::string &annexB, uint32_t, bool keyframe)
{
    if (!keyframe && !haveKeyframe)
        return; // nothing to predict from yet
    if (!ctx) {
        if (!openDecoder(!hardwareFailed) && !openDecoder(false))
            return;
    }
    if (keyframe)
        haveKeyframe = true;

    packet->data = reinterpret_cast<uint8_t *>(const_cast<char *>(annexB.data()));
    packet->size = static_cast<int>(annexB.size());
    int ret = avcodec_send_packet(ctx, packet);
    if (ret < 0) {
        if (++consecutiveErrors >= 3) {
            consecutiveErrors = 0;
            if (hardware) {
                log(LogLevel::Warning, LogStream) << "Hardware decoding keeps failing, switching to software";
                hardwareFailed = true;
                close();
            }
            haveKeyframe = false;
            needKeyframe.publish();
        }
        return;
    }

    while ((ret = avcodec_receive_frame(ctx, frame)) >= 0) {
        consecutiveErrors = 0;
        present(frame);
        av_frame_unref(frame);
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        haveKeyframe = false;
        needKeyframe.publish();
    }
}

void StreamDecoder::present(AVFrame *decoded)
{
    AVFrame *source = decoded;
    if (decoded->format == AV_PIX_FMT_D3D11) {
        av_frame_unref(swFrame);
        if (av_hwframe_transfer_data(swFrame, decoded, 0) < 0)
            return;
        source = swFrame;
    }

    const Platform::Size sourceSize{source->width, source->height};
    const Platform::Size bounds{targetWidth.load(), targetHeight.load()};
    Platform::Size outSize = Platform::fitWithin(sourceSize, bounds);
    if (outSize.width > sourceSize.width)
        outSize = sourceSize;
    outSize.width = std::max(outSize.width, 2);
    outSize.height = std::max(outSize.height, 2);

    sws = sws_getCachedContext(sws, source->width, source->height, static_cast<AVPixelFormat>(source->format),
                               outSize.width, outSize.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws)
        return;

    Platform::ImageBuffer image{outSize.width, outSize.height, 4,
                                std::vector<uint8_t>(static_cast<size_t>(outSize.width) * outSize.height * 4)};
    uint8_t *dst[4] = { image.pixels.data(), nullptr, nullptr, nullptr };
    int dstStride[4] = { static_cast<int>(image.stride()), 0, 0, 0 };
    sws_scale(sws, source->data, source->linesize, 0, source->height, dst, dstStride);
    frameDecoded.publish(image, sourceSize);
}

} // namespace Stream
} // namespace Core
} // namespace Styx

#endif
