#pragma once

#ifdef STYX_HAVE_FFMPEG

#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include <cstdint>
#include <string>

#include <vector>

#include "Core/ProxyConfig.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace Styx {
namespace Core {
namespace Media {

class MediaDecoder
{
public:
    enum class Stream {
        None,
        Video,
        Audio,
    };

    struct Info
    {
        Platform::Size native;
        int64_t durationMs = 0;
        bool seekable = false;
        bool hasAudio = false;
        bool hasVideo = false;
    };

    MediaDecoder() = default;
    ~MediaDecoder();

    MediaDecoder(const MediaDecoder &) = delete;
    MediaDecoder &operator=(const MediaDecoder &) = delete;

    void setInterrupt(int (*callback)(void *), void *opaque);

    bool open(const std::string &input, const ProxyConfig &proxy, std::string *error);
    void close();

    [[nodiscard]] Info info() const;
    [[nodiscard]] bool hasVideo() const { return videoStream >= 0; }
    [[nodiscard]] bool hasAudio() const { return audioStream >= 0; }
    [[nodiscard]] int64_t containerDurationMs() const;

    int readPacket(AVPacket *packet);
    [[nodiscard]] Stream streamOf(const AVPacket *packet) const;
    [[nodiscard]] int64_t packetTimestampMs(const AVPacket *packet) const;

    bool seek(int64_t targetMs, std::string *error);
    void flush();

    bool sendPacket(Stream stream, AVPacket *packet);
    bool drain(Stream stream) { return sendPacket(stream, nullptr); }
    int receiveFrame(Stream stream, AVFrame *frame);
    [[nodiscard]] int64_t frameTimestampMs(const AVFrame *frame, Stream stream) const;

    Platform::ImageBuffer scaleToImage(const AVFrame *frame, const Platform::Size &target);

    int appendResampledFrames(const AVFrame *frame, std::vector<float> &out, std::vector<float> &scratch);

    [[nodiscard]] static std::string errorString(int code);

private:
    bool openCodec(int streamIndex, AVCodecContext **out);
    bool openResampler();
    [[nodiscard]] AVCodecContext *contextFor(Stream stream) const;
    [[nodiscard]] int streamIndexFor(Stream stream) const;

    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *videoCtx = nullptr;
    AVCodecContext *audioCtx = nullptr;
    SwsContext *swsCtx = nullptr;
    SwrContext *swrCtx = nullptr;
    int videoStream = -1;
    int audioStream = -1;
    Platform::Size swsTarget;

    static int denyNestedOpen(AVFormatContext *, AVIOContext **, const char *url, int, AVDictionary **);

    int (*interruptCallback)(void *) = nullptr;
    void *interruptOpaque = nullptr;

    AVIOContext *curlIo = nullptr;
};

} // namespace Media
} // namespace Core
} // namespace Styx

#endif // STYX_HAVE_FFMPEG
