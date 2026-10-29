#include "VideoEncoder.hpp"

#if defined(_WIN32) && defined(STYX_HAVE_FFMPEG)

#include "D3DDevice.hpp"
#include "SpsVuiRewriter.hpp"

#include "Core/Logging.hpp"

#include <string_view>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace Styx {
namespace Core {
namespace Stream {

namespace {

std::string averror(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// NAL types present in an Annex B buffer
struct NalScan
{
    bool sps = false;
    bool pps = false;
    bool idr = false;
};

NalScan scanNals(const std::string &data)
{
    NalScan scan;
    const auto *p = reinterpret_cast<const uint8_t *>(data.data());
    const size_t size = data.size();
    for (size_t i = 0; i + 3 < size; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            const uint8_t type = p[i + 3] & 0x1F;
            scan.sps |= type == 7;
            scan.pps |= type == 8;
            scan.idr |= type == 5;
            i += 2;
        }
    }
    return scan;
}

// Leading SPS/PPS NALs of a keyframe, start codes included.
std::string extractParameterSets(const std::string &data)
{
    const auto *p = reinterpret_cast<const uint8_t *>(data.data());
    const size_t size = data.size();
    size_t i = 0;
    while (i + 3 < size) {
        if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && i + 4 < size && p[i + 3] == 1))) {
            const size_t header = p[i + 2] == 1 ? i + 3 : i + 4;
            const uint8_t type = p[header] & 0x1F;
            if (type != 7 && type != 8 && type != 9)
                return data.substr(0, i);
            i = header;
        } else {
            ++i;
        }
    }
    return data;
}

void setOption(AVCodecContext *ctx, const char *key, const char *value)
{
    // Options differ per encoder and between FFmpeg versions; an unknown one is not fatal.
    if (av_opt_set(ctx->priv_data, key, value, 0) < 0)
        log(LogLevel::Debug, LogStream) << "Encoder option not supported:" << key << "=" << value;
}

void lockDevice(void *ctx)
{
    static_cast<std::recursive_mutex *>(ctx)->lock();
}

void unlockDevice(void *ctx)
{
    static_cast<std::recursive_mutex *>(ctx)->unlock();
}

} // namespace

VideoEncoder::VideoEncoder(std::shared_ptr<D3DDevice> dev) : device(std::move(dev))
{
}

VideoEncoder::~VideoEncoder()
{
    close();
}

void VideoEncoder::close()
{
    avcodec_free_context(&ctx);
    av_buffer_unref(&hwFrames);
    av_buffer_unref(&hwDevice);
    sws_freeContext(sws);
    sws = nullptr;
    encoderName.clear();
    gpuInput = false;
    parameterSets.clear();
}

bool VideoEncoder::open(const EncoderSettings &settings, std::string *error)
{
    close();
    current = settings;
    // H264 needs even dimensions for 4:2:0
    current.size = {current.size.width & ~1, current.size.height & ~1};

    struct Candidate
    {
        const char *name;
        bool gpu;
    };
    static const Candidate candidates[] = {
        { "h264_amf", true },
        { "h264_nvenc", true },
        { "h264_qsv", false },
        { "h264_mf", false },
        { "libopenh264", false },
        { "libx264", false },
    };

    std::string failures;
    for (const auto &candidate : candidates) {
        if (!avcodec_find_encoder_by_name(candidate.name))
            continue;
        std::string reason;
        if (tryOpen(candidate.name, candidate.gpu, &reason)) {
            log(LogLevel::Info, LogStream) << "Video encoder:" << encoderName << (gpuInput ? "(GPU surfaces)" : "(CPU frames)")
                << current.size.width << "x" << current.size.height << "@" << current.framerate << "fps" << current.bitrate / 1000 << "kbps";
            return true;
        }
        if (!failures.empty())
            failures += "; ";
        failures += std::string(candidate.name) + ": " + reason;
        close();
    }
    if (error)
        *error = failures.empty() ? "no H264 encoder in this FFmpeg build" : failures;
    return false;
}

bool VideoEncoder::tryOpen(const char *name, bool gpu, std::string *error)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        *error = "out of memory";
        return false;
    }

    ctx->width = current.size.width;
    ctx->height = current.size.height;
    ctx->time_base = AVRational{ 1, current.framerate };
    ctx->framerate = AVRational{ current.framerate, 1 };
    ctx->gop_size = current.framerate * 5;
    ctx->max_b_frames = 0;
    ctx->bit_rate = current.bitrate;
    ctx->rc_max_rate = current.maxBitrate;
    ctx->rc_buffer_size = current.maxBitrate / 2;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->color_range = AVCOL_RANGE_MPEG;
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;
    ctx->profile = AV_PROFILE_H264_MAIN;

    const std::string_view encoder(name);
    if (encoder == "h264_amf") {
        setOption(ctx, "usage", "ultralowlatency");
        setOption(ctx, "rc", "cbr");
        setOption(ctx, "quality", "speed");
        setOption(ctx, "forced_idr", "1");
        setOption(ctx, "aud", "0");
        setOption(ctx, "bf", "0");
        setOption(ctx, "enforce_hrd", "1");
        setOption(ctx, "filler_data", "0");
    } else if (encoder == "h264_nvenc") {
        setOption(ctx, "preset", "p4");
        setOption(ctx, "tune", "ull");
        setOption(ctx, "rc", "cbr");
        setOption(ctx, "zerolatency", "1");
        setOption(ctx, "forced-idr", "1");
        setOption(ctx, "delay", "0");
    } else if (encoder == "h264_qsv") {
        setOption(ctx, "preset", "veryfast");
        setOption(ctx, "forced_idr", "1");
        setOption(ctx, "async_depth", "1");
        setOption(ctx, "look_ahead", "0");
    } else if (encoder == "h264_mf") {
        setOption(ctx, "rate_control", "cbr");
        setOption(ctx, "scenario", "display_remoting");
        setOption(ctx, "hw_encoding", "1");
    } else if (encoder == "libopenh264") {
        setOption(ctx, "allow_skip_frames", "1");
        ctx->profile = AV_PROFILE_UNKNOWN; // OpenH264 picks constrained baseline itself
    } else if (encoder == "libx264") {
        setOption(ctx, "preset", "veryfast");
        setOption(ctx, "tune", "zerolatency");
    }

    if (gpu) {
        hwDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        auto *deviceCtx = reinterpret_cast<AVHWDeviceContext *>(hwDevice->data);
        auto *d3d = static_cast<AVD3D11VADeviceContext *>(deviceCtx->hwctx);
        d3d->device = device->device();
        d3d->device->AddRef();
        d3d->lock = lockDevice;
        d3d->unlock = unlockDevice;
        d3d->lock_ctx = &device->mutex();
        int ret = av_hwdevice_ctx_init(hwDevice);
        if (ret < 0) {
            *error = "hw device: " + averror(ret);
            return false;
        }

        hwFrames = av_hwframe_ctx_alloc(hwDevice);
        auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(hwFrames->data);
        framesCtx->format = AV_PIX_FMT_D3D11;
        framesCtx->sw_format = AV_PIX_FMT_NV12;
        framesCtx->width = ctx->width;
        framesCtx->height = ctx->height;
        // 0 = one texture per frame: NV12 render-target texture arrays are refused by some
        // drivers (E_INVALIDARG), single textures work everywhere
        framesCtx->initial_pool_size = 0;
        static_cast<AVD3D11VAFramesContext *>(framesCtx->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
        ret = av_hwframe_ctx_init(hwFrames);
        if (ret < 0) {
            *error = "hw frames: " + averror(ret);
            return false;
        }

        ctx->pix_fmt = AV_PIX_FMT_D3D11;
        ctx->hw_frames_ctx = av_buffer_ref(hwFrames);
        ctx->hw_device_ctx = av_buffer_ref(hwDevice);
    } else {
        // NV12 if the encoder takes it, otherwise its first 4:2:0 format
        const void *configs = nullptr;
        int count = 0;
        cpuFormat = AV_PIX_FMT_NONE;
        if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) >= 0 && configs) {
            const auto *formats = static_cast<const AVPixelFormat *>(configs);
            for (int i = 0; i < count; ++i) {
                if (formats[i] == AV_PIX_FMT_NV12) {
                    cpuFormat = AV_PIX_FMT_NV12;
                    break;
                }
                if (cpuFormat == AV_PIX_FMT_NONE && formats[i] == AV_PIX_FMT_YUV420P)
                    cpuFormat = AV_PIX_FMT_YUV420P;
            }
        } else {
            cpuFormat = AV_PIX_FMT_NV12;
        }
        if (cpuFormat == AV_PIX_FMT_NONE) {
            *error = "takes neither NV12 nor YUV420P";
            return false;
        }
        ctx->pix_fmt = cpuFormat;
        if (cpuFormat != AV_PIX_FMT_NV12) {
            sws = sws_getContext(ctx->width, ctx->height, AV_PIX_FMT_NV12, ctx->width, ctx->height, cpuFormat,
                                 SWS_POINT, nullptr, nullptr, nullptr);
            if (!sws) {
                *error = "no NV12 converter";
                return false;
            }
        }
    }

    const int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
        *error = averror(ret);
        return false;
    }

    encoderName = name;
    gpuInput = gpu;
    return true;
}

AVFrame *VideoEncoder::acquireGpuFrame(ID3D11Texture2D **texture, UINT *arrayIndex)
{
    if (!hwFrames)
        return nullptr;
    AVFrame *frame = av_frame_alloc();
    if (av_hwframe_get_buffer(hwFrames, frame, 0) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    *texture = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
    *arrayIndex = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
    return frame;
}

AVFrame *VideoEncoder::acquireCpuFrame()
{
    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_NV12;
    frame->width = current.size.width;
    frame->height = current.size.height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    return frame;
}

bool VideoEncoder::encode(AVFrame *frame, int64_t ptsUs, bool forceKeyframe, std::vector<EncodedPacket> &out)
{
    if (!ctx) {
        av_frame_free(&frame);
        return false;
    }

    if (sws && frame->format == AV_PIX_FMT_NV12) {
        AVFrame *converted = av_frame_alloc();
        converted->format = cpuFormat;
        converted->width = frame->width;
        converted->height = frame->height;
        if (av_frame_get_buffer(converted, 32) < 0) {
            av_frame_free(&converted);
            av_frame_free(&frame);
            return false;
        }
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, converted->data, converted->linesize);
        av_frame_free(&frame);
        frame = converted;
    }

    frame->pts = av_rescale_q(ptsUs, AVRational{ 1, 1000000 }, ctx->time_base);
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    if (forceKeyframe) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
    } else {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
    }

    const int ret = avcodec_send_frame(ctx, frame);
    av_frame_free(&frame);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        log(LogLevel::Warning, LogStream) << "avcodec_send_frame:" << averror(ret);
        return false;
    }
    return drain(out);
}

bool VideoEncoder::drain(std::vector<EncodedPacket> &out)
{
    AVPacket *packet = av_packet_alloc();
    for (;;) {
        const int ret = avcodec_receive_packet(ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            log(LogLevel::Warning, LogStream) << "avcodec_receive_packet:" << averror(ret);
            av_packet_free(&packet);
            return false;
        }

        EncodedPacket encoded;
        encoded.data.assign(reinterpret_cast<const char *>(packet->data), packet->size);
        encoded.ptsUs = av_rescale_q(packet->pts, ctx->time_base, AVRational{ 1, 1000000 });

        const NalScan scan = scanNals(encoded.data);
        encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) || scan.idr;
        if (encoded.keyframe) {
            if (scan.sps && scan.pps) {
                parameterSets = extractParameterSets(encoded.data);
            } else if (!parameterSets.empty()) {
                encoded.data.insert(0, parameterSets);
            } else if (ctx->extradata_size > 0) {
                encoded.data.insert(0, reinterpret_cast<const char *>(ctx->extradata), ctx->extradata_size);
            }
            // viewers' decoders must not hold frames back for reordering; libwebrtc does the same
            encoded.data = rewriteAccessUnitSps(encoded.data);
        }

        out.push_back(std::move(encoded));
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    return true;
}

} // namespace Stream
} // namespace Core
} // namespace Styx

#endif
