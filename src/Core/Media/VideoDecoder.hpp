// Ported from ouwou upstream/video-streams (71b4907); original implementation retained.
#pragma once
#include "Core/Media/CodecOffer.hpp"
#include "Core/Video/VideoTypes.hpp"
#include "Discord/Voice/VideoReceiver.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
struct AVBufferRef; struct AVCodec; struct AVCodecContext; struct AVFrame; struct AVPacket;
namespace Styx::Core::Media {
using FrameSize = Video::FrameSize;
[[nodiscard]] bool hardwareDecodingEnabled(); void setHardwareDecodingEnabled(bool enabled);
[[nodiscard]] std::vector<Discord::Voice::VideoCodec> decodableVideoCodecs();
[[nodiscard]] std::vector<CodecOffer> offeredVideoCodecs(const std::vector<Discord::Voice::VideoCodec> &encodable = {});
class VideoFrameSink { public: virtual ~VideoFrameSink() = default; virtual void present(const AVFrame *frame) = 0; [[nodiscard]] virtual bool acceptsHardwareFrames(int) const { return false; } };
class VideoDecoder {
public:
    struct Stats { std::string decoderName; int width = 0; int height = 0; uint64_t decoded = 0, dropped = 0, errors = 0, queuesDiscarded = 0; int backlogMs = 0; double averageDecodeMs = 0; bool hardware = false; };
    explicit VideoDecoder(bool allowHardware = hardwareDecodingEnabled(), int softwareThreads = MAX_SOFTWARE_THREADS); ~VideoDecoder();
    void setSink(std::shared_ptr<VideoFrameSink> sink); void setKeyframeRequester(std::function<void()> requester); void setCodecFailureHandler(std::function<void(Discord::Voice::VideoCodec)> handler); void push(Discord::Voice::VideoCodec codec, Discord::Voice::EncodedVideoFrame frame); [[nodiscard]] Stats stats() const;
    static constexpr int MAX_QUEUE_AGE_MS = 1500, MAX_QUEUED_FRAMES = 90, MAX_SOFTWARE_THREADS = 4, HARDWARE_TRIAL_FRAMES = 30, EXTRA_HARDWARE_FRAMES = 4;
private:
    struct Queued { Discord::Voice::VideoCodec codec; Discord::Voice::EncodedVideoFrame frame; std::chrono::steady_clock::time_point queuedAt; };
    void run(); bool open(Discord::Voice::VideoCodec); bool openHardware(Discord::Voice::VideoCodec); bool openSoftware(Discord::Voice::VideoCodec); void close(); void decode(Queued &); [[nodiscard]] bool decodeWithOpenDecoder(const Discord::Voice::EncodedVideoFrame &, int); void startOverAtNextKeyframe(); void reportUndecodableFrame(); void describeDecoder();
    mutable std::mutex mutex; std::condition_variable wake; std::deque<Queued> queue; bool queueWasDiscarded = false, stopping = false; std::shared_ptr<VideoFrameSink> frameSink; std::function<void()> keyframeRequester; std::function<void(Discord::Voice::VideoCodec)> codecFailureHandler; Stats counters; AVCodecContext *context = nullptr; AVBufferRef *hardwareDevice = nullptr; AVFrame *decoded = nullptr; AVFrame *downloaded = nullptr; AVPacket *packet = nullptr; Discord::Voice::VideoCodec openCodec = Discord::Voice::VideoCodec::Unknown; int hardwarePixelFormat = -1; const char *hardwareName = nullptr; bool hardwareAllowed; int softwareThreads; bool hardwareRuledOut = false; uint64_t framesSinceOpen = 0; bool awaitingKeyframe = true; std::thread thread;
};
} // namespace Styx::Core::Media
