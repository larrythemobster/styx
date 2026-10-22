// Ported from ouwou upstream/video-streams (71b4907); original implementation retained.
#pragma once

#include "Core/Video/VideoTypes.hpp"
#include "Discord/Voice/VideoReceiver.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct AVBSFContext;
struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace Styx::Core::Media {
using FrameSize = Video::FrameSize;
struct VideoEncoderCandidate;
[[nodiscard]] std::vector<Discord::Voice::VideoCodec> encodableVideoCodecs();
[[nodiscard]] std::vector<std::string> videoEncoderNames(Discord::Voice::VideoCodec codec);
[[nodiscard]] bool videoEncoderWorks(const std::string &name);
[[nodiscard]] std::string videoEncoderFor(Discord::Voice::VideoCodec codec);
[[nodiscard]] FrameSize codedSize(const std::string &encoderName, const FrameSize &size);
[[nodiscard]] FrameSize av1CodedSize(const std::vector<uint8_t> &keyFrame);
struct Av1SizeProbe { FrameSize given; FrameSize coded; };
[[nodiscard]] bool codesEverySizeAsItIs(const std::vector<Av1SizeProbe> &probes);
inline constexpr int MOST_ROWS_ADDED = 8;
[[nodiscard]] bool paddingIsAcceptable(const FrameSize &size, const FrameSize &coded);
[[nodiscard]] bool sendCodecAllowed(Discord::Voice::VideoCodec codec);
void setSendCodecAllowed(Discord::Voice::VideoCodec codec, bool allowed);
[[nodiscard]] std::vector<Discord::Voice::VideoCodec> allowedSendCodecs(const std::vector<Discord::Voice::VideoCodec> &encodable);
[[nodiscard]] std::vector<Discord::Voice::VideoCodec> shareCodecs(const std::vector<Discord::Voice::VideoCodec> &encodable, const std::string &av1Encoder, const FrameSize &size);
[[nodiscard]] std::vector<Discord::Voice::VideoCodec> cameraCodecs(const std::vector<Discord::Voice::VideoCodec> &encodable, const std::string &av1Encoder);
class EncodedFrameFinisher {
public:
    explicit EncodedFrameFinisher(Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown) : codec(codec) {}
    void rememberParameterSets(const uint8_t *extradata, size_t size);
    [[nodiscard]] bool finish(Discord::Voice::EncodedVideoFrame &frame);
private:
    [[nodiscard]] bool finishAnnexB(Discord::Voice::EncodedVideoFrame &frame);
    [[nodiscard]] bool finishAv1(Discord::Voice::EncodedVideoFrame &frame);
    Discord::Voice::VideoCodec codec;
    std::array<std::vector<uint8_t>, 3> parameterSets;
    std::vector<uint8_t> sequenceHeader;
};
class VideoEncoder {
public:
    enum class Content { Screen, Camera };
    struct Settings { Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown; int framerate = 30; int bitrate = 0; int keyframeIntervalMs = 0; Content content = Content::Screen; std::string encoderName; };
    enum class Outcome { Encoded, CardFramesRefused, Failed };
    struct Stats { std::string encoderName; bool hardware = false; int width = 0; int height = 0; uint64_t encoded = 0; uint64_t opened = 0; double averageEncodeMs = 0; };
    explicit VideoEncoder(const Settings &settings); ~VideoEncoder();
    VideoEncoder(const VideoEncoder &) = delete; VideoEncoder &operator=(const VideoEncoder &) = delete;
    [[nodiscard]] bool hasEncoder() const { return candidate != nullptr; }
    [[nodiscard]] FrameSize codedSize(const FrameSize &size) const;
    [[nodiscard]] bool takesCardFrames() const; [[nodiscard]] bool takesRgbCardFrames() const; [[nodiscard]] int softwareFormat() const;
    void requestKeyframe(); void setBitrate(int bitrate);
    Outcome encode(AVFrame *frame, int64_t timestamp, std::vector<Discord::Voice::EncodedVideoFrame> &out);
    [[nodiscard]] const Stats &stats() const { return counters; }
    static constexpr int TRIAL_FRAMES = 30, OUTPUT_WAIT_MS = 250;
private:
    bool encodeWithOpenEncoder(AVFrame *, int64_t, std::vector<Discord::Voice::EncodedVideoFrame> &); bool open(const AVFrame *); AVCodecContext *configured(const AVFrame *, int) const; void openColourDescriber(const AVFrame *); void describeColours(); bool prepareUpload(const AVFrame *); AVFrame *uploadedCopy(const AVFrame *); void close(); void moveToNextEncoder(); bool collect(std::vector<Discord::Voice::EncodedVideoFrame> &); [[nodiscard]] int bufferBits() const;
    Settings settings; const VideoEncoderCandidate *candidate = nullptr; AVCodecContext *context = nullptr; AVBufferRef *uploadFrames = nullptr; AVFrame *uploaded = nullptr; AVPacket *packet = nullptr; AVBSFContext *colourDescriber = nullptr; AVPacket *asEncoded = nullptr; bool cardFramesRefused = false, keyframeWanted = false, reopenWanted = false; int bitrateChosenFor = 0, awaited = 0; uint64_t framesSinceOpen = 0; int64_t lastKeyframeAt = 0; EncodedFrameFinisher finisher; Stats counters;
};
} // namespace Styx::Core::Media
