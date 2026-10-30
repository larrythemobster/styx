#include "Core/Media/ClipDecoder.hpp"

#include "Core/AnimationTiming.hpp"

#include <stb_image_resize2.h>
#include <webp/demux.h>

#include <algorithm>
#include <cstring>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

#ifdef STYX_HAVE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#endif // STYX_HAVE_FFMPEG

namespace Styx {
namespace Core {
namespace Media {

namespace {

enum class Container {
    Unknown,
    Gif,
    Mp4,
    WebM,
    WebP,
};

constexpr int SniffedBytes = 12;

size_t bytes(Platform::Size size)
{
    return static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4;
}

void storeFrame(Platform::ImageBuffer &frame, const uint8_t *pixels,
                Platform::Size sourceSize, Platform::Size frameSize)
{
    if (!pixels || sourceSize.empty() || frameSize.empty()) {
        frame = {};
        return;
    }

    frame.width = frameSize.width;
    frame.height = frameSize.height;
    frame.channels = 4;
    frame.pixels.resize(bytes(frameSize));
    if (sourceSize.width == frameSize.width && sourceSize.height == frameSize.height) {
        std::memcpy(frame.pixels.data(), pixels, frame.pixels.size());
    } else if (!stbir_resize_uint8_linear(pixels, sourceSize.width, sourceSize.height, 0,
                                          frame.pixels.data(), frameSize.width, frameSize.height, 0,
                                          STBIR_RGBA)) {
        frame = {};
    }
}

std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

Container sniffContainer(const std::string &path)
{
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    char head[SniffedBytes]{};
    file.read(head, sizeof(head));
    const auto size = file.gcount();
    if (size >= 4 && std::memcmp(head, "GIF8", 4) == 0)
        return Container::Gif;
    if (size >= 12 && std::memcmp(head, "RIFF", 4) == 0 && std::memcmp(head + 8, "WEBP", 4) == 0)
        return Container::WebP;
    if (size >= 8 && std::memcmp(head + 4, "ftyp", 4) == 0)
        return Container::Mp4;
    if (size >= 4 && std::memcmp(head, "\x1a\x45\xdf\xa3", 4) == 0)
        return Container::WebM;
    return Container::Unknown;
}

class GifClipDecoder final : public ClipDecoder
{
public:
    static std::unique_ptr<ClipDecoder> open(const std::string &path)
    {
        auto bytes = readFile(path);
        if (bytes.empty() || bytes.size() > static_cast<size_t>(INT_MAX))
            return nullptr;
        auto frames = Platform::Image::decodeGif(bytes.data(), bytes.size());
        if (!frames) return nullptr;
        auto decoder = std::unique_ptr<GifClipDecoder>(new GifClipDecoder);
        decoder->frames = std::move(*frames);
        return decoder;
    }
    Platform::Size nativeSize() const override { return frames.size; }
    bool nextFrameOfLoop(Platform::ImageBuffer &frame, int64_t &startMs, Platform::Size frameSize) override
    {
        if (index >= frames.count)
            return false;
        storeFrame(frame, frames.pixels.get() + bytes(frames.size) * index, frames.size, frameSize);
        startMs = elapsedMs;
        elapsedMs += normalizedFrameDelayMs(frames.delays ? frames.delays[index] : 0);
        ++index;
        return !frame.empty();
    }
    int64_t finishedLoopMs() const override { return elapsedMs; }
    bool rewind() override { index = 0; elapsedMs = 0; return true; }
private:
    Platform::GifFrames frames;
    int index = 0;
    int64_t elapsedMs = 0;
};

class WebpClipDecoder final : public ClipDecoder
{
public:
    static std::unique_ptr<ClipDecoder> open(const std::string &path)
    {
        auto decoder = std::unique_ptr<WebpClipDecoder>(new WebpClipDecoder);
        decoder->bytes = readFile(path);
        if (decoder->bytes.empty())
            return nullptr;
        WebPData data{decoder->bytes.data(), decoder->bytes.size()};
        WebPAnimDecoderOptions options;
        if (!WebPAnimDecoderOptionsInit(&options))
            return nullptr;
        options.color_mode = MODE_RGBA;
        decoder->animation = WebPAnimDecoderNew(&data, &options);
        WebPAnimInfo info{};
        if (!decoder->animation || !WebPAnimDecoderGetInfo(decoder->animation, &info) || info.frame_count < 2)
            return nullptr;
        decoder->size = {static_cast<int>(info.canvas_width), static_cast<int>(info.canvas_height)};
        return decoder;
    }
    ~WebpClipDecoder() override { if (animation) WebPAnimDecoderDelete(animation); }
    Platform::Size nativeSize() const override { return size; }
    bool nextFrameOfLoop(Platform::ImageBuffer &frame, int64_t &startMs, Platform::Size frameSize) override
    {
        uint8_t *pixels = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderHasMoreFrames(animation) || !WebPAnimDecoderGetNext(animation, &pixels, &timestamp))
            return false;
        storeFrame(frame, pixels, size, frameSize);
        startMs = elapsedMs;
        elapsedMs += normalizedFrameDelayMs(timestamp - lastTimestamp);
        lastTimestamp = timestamp;
        return !frame.empty();
    }
    int64_t finishedLoopMs() const override { return elapsedMs; }
    bool rewind() override { WebPAnimDecoderReset(animation); elapsedMs = 0; lastTimestamp = 0; return true; }
private:
    std::vector<uint8_t> bytes;
    WebPAnimDecoder *animation = nullptr;
    Platform::Size size;
    int lastTimestamp = 0;
    int64_t elapsedMs = 0;
};

#ifdef STYX_HAVE_FFMPEG

constexpr int SwscaleOverrunBytes = 64;
constexpr int64_t UnknownLastFrameMs = 100;
constexpr int FramesReadAheadToGuessFrameRate = 0;

class FfmpegClipDecoder final : public ClipDecoder
{
public:
    static std::unique_ptr<ClipDecoder> open(const std::string &path, const char *demuxer)
    {
        auto decoder = std::unique_ptr<FfmpegClipDecoder>(new FfmpegClipDecoder(path, demuxer));
        if (!decoder->decoded || !decoder->packet || !decoder->openInput())
            return nullptr;
        return decoder;
    }

    ~FfmpegClipDecoder() override
    {
        closeInput();
        sws_freeContext(scaler);
        av_frame_free(&decoded);
        av_packet_free(&packet);
    }

    Platform::Size nativeSize() const override { return {codec->width, codec->height}; }

    bool nextFrameOfLoop(Platform::ImageBuffer &frame, int64_t &startMs, Platform::Size frameSize) override
    {
        if (!receiveFrame())
            return false;

        const int64_t pts = decoded->best_effort_timestamp;
        if (pts != AV_NOPTS_VALUE) {
            if (!firstPts)
                firstPts = pts;
            startMs = std::max(lastStartMs, toMs(pts - *firstPts));
        } else {
            startMs = lastStartMs;
        }
        lastGapMs = startMs - lastStartMs;
        lastStartMs = startMs;

        const bool converted = convert(frame, frameSize);
        av_frame_unref(decoded);
        return converted;
    }

    int64_t finishedLoopMs() const override
    {
        const int64_t demuxedMs = firstPts && streamEndPts ? toMs(*streamEndPts - *firstPts) : 0;
        if (demuxedMs > lastStartMs)
            return demuxedMs;
        return lastStartMs + (lastGapMs > 0 ? lastGapMs : UnknownLastFrameMs);
    }

    bool rewind() override
    {
        draining = false;
        lastStartMs = 0;
        lastGapMs = 0;

        avcodec_flush_buffers(codec);
        if (av_seek_frame(format, videoStream, firstPts.value_or(0), AVSEEK_FLAG_BACKWARD) >= 0)
            return true;

        closeInput();
        return openInput();
    }

private:
    FfmpegClipDecoder(const std::string &path, const char *demuxer)
        : path(path), demuxer(demuxer), decoded(av_frame_alloc()), packet(av_packet_alloc())
    {
    }

    bool openInput()
    {
        AVDictionary *options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        const int opened = avformat_open_input(&format, path.c_str(), av_find_input_format(demuxer), &options);
        av_dict_free(&options);
        if (opened < 0)
            return false;

        format->fps_probe_size = FramesReadAheadToGuessFrameRate;
        if (avformat_find_stream_info(format, nullptr) < 0)
            return false;

        videoStream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStream < 0)
            return false;

        for (unsigned i = 0; i < format->nb_streams; ++i) {
            if (int(i) != videoStream)
                format->streams[i]->discard = AVDISCARD_ALL;
        }

        const AVStream *stream = format->streams[videoStream];
        timeBase = stream->time_base;

        const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder)
            return false;

        codec = avcodec_alloc_context3(decoder);
        if (!codec || avcodec_parameters_to_context(codec, stream->codecpar) < 0)
            return false;

        codec->thread_count = 1;
        return avcodec_open2(codec, decoder, nullptr) >= 0 && codec->width > 0 && codec->height > 0;
    }

    void closeInput()
    {
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        videoStream = -1;
    }

    [[nodiscard]] int64_t toMs(int64_t streamTicks) const { return av_rescale_q(streamTicks, timeBase, AVRational{ 1, 1000 }); }

    void feedDecoder()
    {
        for (;;) {
            if (av_read_frame(format, packet) < 0) {
                avcodec_send_packet(codec, nullptr);
                draining = true;
                return;
            }

            const bool wanted = packet->stream_index == videoStream;
            if (wanted && packet->pts != AV_NOPTS_VALUE)
                streamEndPts = std::max(streamEndPts.value_or(packet->pts), packet->pts + std::max<int64_t>(packet->duration, 0));

            const int sent = wanted ? avcodec_send_packet(codec, packet) : AVERROR(EAGAIN);
            av_packet_unref(packet);
            if (sent >= 0)
                return;
        }
    }

    bool receiveFrame()
    {
        for (;;) {
            const int received = avcodec_receive_frame(codec, decoded);
            if (received == 0)
                return true;
            if (received != AVERROR(EAGAIN) || draining)
                return false;
            feedDecoder();
        }
    }

    bool convert(Platform::ImageBuffer &frame, Platform::Size frameSize)
    {
        if (frameSize.empty())
            return false;
        const size_t bytes = static_cast<size_t>(frameSize.width) * frameSize.height * 4;
        frame.width = frameSize.width;
        frame.height = frameSize.height;
        frame.channels = 4;
        frame.pixels.resize(bytes + SwscaleOverrunBytes);
        scaler = sws_getCachedContext(scaler, decoded->width, decoded->height, AVPixelFormat(decoded->format),
                                      frameSize.width, frameSize.height, AV_PIX_FMT_RGBA,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler)
            return false;
        uint8_t *planes[4] = {frame.pixels.data(), nullptr, nullptr, nullptr};
        int strides[4] = {frameSize.width * 4, 0, 0, 0};
        sws_scale(scaler, decoded->data, decoded->linesize, 0, decoded->height, planes, strides);
        frame.pixels.resize(bytes);
        return true;
    }

    std::string path;
    const char *demuxer;

    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    SwsContext *scaler = nullptr;
    AVFrame *decoded;
    AVPacket *packet;
    int videoStream = -1;
    AVRational timeBase{ 1, 1000 };

    std::optional<int64_t> firstPts;
    std::optional<int64_t> streamEndPts;
    int64_t lastStartMs = 0;
    int64_t lastGapMs = 0;
    bool draining = false;
};

std::unique_ptr<ClipDecoder> openWithFfmpeg(const std::string &path, Container container)
{
    switch (container) {
    case Container::Gif:
        return FfmpegClipDecoder::open(path, "gif");
    case Container::Mp4:
        return FfmpegClipDecoder::open(path, "mov");
    case Container::WebM:
        return FfmpegClipDecoder::open(path, "matroska");
    case Container::WebP:
    case Container::Unknown:
        break;
    }
    return nullptr;
}

#else // STYX_HAVE_FFMPEG

std::unique_ptr<ClipDecoder> openWithFfmpeg(const std::string &, Container)
{
    return nullptr;
}

#endif // STYX_HAVE_FFMPEG

} // namespace

std::unique_ptr<ClipDecoder> ClipDecoder::open(const std::string &path)
{
    const Container container = sniffContainer(path);
    if (auto decoder = openWithFfmpeg(path, container))
        return decoder;

    if (container == Container::Gif)
        return GifClipDecoder::open(path);
    if (container == Container::WebP)
        return WebpClipDecoder::open(path);
    return nullptr;
}

} // namespace Media
} // namespace Core
} // namespace Styx
