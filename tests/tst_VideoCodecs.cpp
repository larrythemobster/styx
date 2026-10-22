// Behavioral coverage for the Qt-free upstream/video-streams codec port.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Core/Media/H264Sps.hpp"
#include "Core/Media/HardwareDevices.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Discord/Voice/VideoPacketizer.hpp"
#include "Discord/Voice/VideoReceiver.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <string_view>

using namespace Styx;
using namespace Styx::Core::Media;
using namespace Styx::Discord::Voice;
namespace {
std::vector<uint8_t> h264Keyframe()
{
    std::vector<uint8_t> frame = {0,0,0,1,0x67,0x42,0,0x1e,0xf4,0x0b,0x04,0,0,0,1,0x68,0xce,0x06,0xe2,0,0,0,1,0x65,0x88};
    // An IDR slice larger than the payload limit below, so that it goes out in fragments.
    for (int i = 0; i < 64; ++i)
        frame.push_back(static_cast<uint8_t>(0x10 + i));
    return frame;
}
struct PresentedFrame {
    std::mutex mutex;
    std::condition_variable delivered;
    int width = 0;
    int height = 0;
    int64_t timestamp = AV_NOPTS_VALUE;
    uint8_t luma = 0;
    int frames = 0;
};
class CollectingSink final : public VideoFrameSink {
public:
    explicit CollectingSink(std::shared_ptr<PresentedFrame> result) : result(std::move(result)) {}
    void present(const AVFrame *frame) override
    {
        std::lock_guard lock(result->mutex);
        result->width = frame->width;
        result->height = frame->height;
        result->timestamp = frame->pts;
        result->luma = frame->data[0][0];
        ++result->frames;
        result->delivered.notify_all();
    }
private:
    std::shared_ptr<PresentedFrame> result;
};
PacketPool::Handle packet(PacketPool &pool, std::span<const uint8_t> bytes)
{
    auto result = pool.acquire(); REQUIRE(result->append(bytes)); return result;
}
}

TEST_CASE("AV1 from AMF is coded in whole blocks", "[video][codec]")
{
    const std::string amf = "av1_amf";
    REQUIRE(codedSize(amf, {1920, 1080}) == FrameSize{1920, 1082});
    REQUIRE(codedSize(amf, {640, 360}) == FrameSize{640, 362});
    REQUIRE(codedSize(amf, {960, 540}) == FrameSize{960, 544});
    REQUIRE(codedSize(amf, {1000, 700}) == FrameSize{1024, 704});
    REQUIRE(codedSize(amf, {1280, 720}) == FrameSize{1280, 720});
    REQUIRE(codedSize(amf, {2560, 1440}) == FrameSize{2560, 1440});
    REQUIRE(codedSize(amf, {1452, 752}) == FrameSize{1472, 752});

    // Every other encoder codes what it is given, but for VAAPI's, which is asked on each machine.
    std::vector<std::string> others = {std::string(), "av1_nvenc", "av1_qsv", "av1_mf hardware"};
    for (VideoCodec codec : {VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8}) {
        const auto names = videoEncoderNames(codec);
        others.insert(others.end(), names.begin(), names.end());
    }
    std::erase(others, amf);
    std::erase(others, std::string("av1_vaapi"));
    for (const auto &name : others) {
        INFO(name);
        for (FrameSize size : {FrameSize{1920, 1080}, FrameSize{640, 360}, FrameSize{1000, 700}, FrameSize{1452, 752}})
            REQUIRE(codedSize(name, size) == size);
    }
}

TEST_CASE("Black columns are not acceptable and a few rows are", "[video][codec]")
{
    REQUIRE(paddingIsAcceptable({1280, 720}, {1280, 720}));
    REQUIRE(paddingIsAcceptable({1280, 720}, {1280, 728}));
    REQUIRE_FALSE(paddingIsAcceptable({1280, 720}, {1280, 730}));
    REQUIRE_FALSE(paddingIsAcceptable({1280, 720}, {1282, 720}));
    REQUIRE_FALSE(paddingIsAcceptable({1280, 720}, {1344, 722}));

    const auto accepted = [](const std::string &encoder, FrameSize size) { return paddingIsAcceptable(size, codedSize(encoder, size)); };
    const std::string amf = "av1_amf";
    REQUIRE(accepted(amf, {1920, 1080}));
    REQUIRE(accepted(amf, {640, 360}));
    REQUIRE(accepted(amf, {960, 540}));
    REQUIRE(accepted(amf, {1280, 720}));
    REQUIRE(accepted(amf, {2560, 1440}));
    REQUIRE_FALSE(accepted(amf, {1000, 700}));
    REQUIRE_FALSE(accepted(amf, {1452, 752}));
    // The right width, and fourteen rows short of a whole block.
    REQUIRE_FALSE(accepted(amf, {1280, 722}));

    for (const std::string exact : {"av1_nvenc", "av1_qsv", "hevc_amf", "libvpx"}) {
        INFO(exact);
        for (FrameSize size : {FrameSize{1920, 1080}, FrameSize{1000, 700}, FrameSize{1452, 752}, FrameSize{1280, 722}})
            REQUIRE(accepted(exact, size));
    }
}

TEST_CASE("AV1 is offered only where it adds no black", "[video][codec]")
{
    const std::string amf = "av1_amf";
    const std::string nvenc = "av1_nvenc";
    const std::vector<VideoCodec> all = {VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8};
    const std::vector<VideoCodec> withoutAv1 = {VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8};

    REQUIRE(shareCodecs(all, amf, {1920, 1080}) == all);
    REQUIRE(shareCodecs(all, amf, {1280, 720}) == all);
    REQUIRE(shareCodecs(all, amf, {1452, 752}) == withoutAv1);
    REQUIRE(shareCodecs(all, amf, {1000, 700}) == withoutAv1);
    REQUIRE(shareCodecs(all, nvenc, {1452, 752}) == all);
    REQUIRE(shareCodecs(withoutAv1, amf, {1452, 752}) == withoutAv1);
    // Asked of what a share already offers, a codec that was left out once stays out.
    REQUIRE(shareCodecs(shareCodecs(all, amf, {1452, 752}), amf, {1920, 1080}) == withoutAv1);

    REQUIRE(cameraCodecs(all, amf) == withoutAv1);
    REQUIRE(cameraCodecs(all, nvenc) == all);
    REQUIRE(cameraCodecs(all, "av1_qsv") == all);
    REQUIRE(cameraCodecs(withoutAv1, amf) == withoutAv1);
    REQUIRE(cameraCodecs({VideoCodec::VP8}, amf) == std::vector<VideoCodec>{VideoCodec::VP8});
}

TEST_CASE("H264 keyframe completion keeps parameter sets", "[video][codec]")
{

    EncodedFrameFinisher finisher(VideoCodec::H264);
    EncodedVideoFrame full{.data = h264Keyframe()};
    REQUIRE(finisher.finish(full)); REQUIRE(full.keyframe);
    EncodedVideoFrame idr{.data = {0,0,0,1,0x65,0x80,0,0}};
    REQUIRE(finisher.finish(idr)); REQUIRE(idr.keyframe);
    REQUIRE(idr.data.size() > 4);
}

TEST_CASE("Video receiver reassembles frames and reports packet loss", "[video][receiver]")
{
    VideoReceiver receiver(42, VideoCodec::H264); PacketPool pool;
    VideoPacketizer packetizer(VideoCodec::H264);
    const auto payloads = packetizer.packetize(h264Keyframe(), true, 16);
    REQUIRE(payloads.size() > 2);
    receiver.insert(100, 9000, false, packet(pool, payloads[0]), 0);
    receiver.insert(102, 9000, false, packet(pool, payloads[2]), 0);
    auto feedback = receiver.poll(VideoReceiver::REORDER_GRACE_MS + 1);
    REQUIRE(feedback.nacks == std::vector<uint16_t>{101});
    receiver.insert(101, 9000, false, packet(pool, payloads[1]), 20, true);
    for (size_t i = 3; i < payloads.size(); ++i) receiver.insert(static_cast<uint16_t>(100 + i), 9000, i + 1 == payloads.size(), packet(pool, payloads[i]), 20);
    const auto frames = receiver.takeFrames(); REQUIRE(frames.size() == 1); REQUIRE(frames[0].keyframe);
}

TEST_CASE("Native VP8 encoder and decoder preserve a synthetic frame", "[video][codec]")
{
    VideoEncoder::Settings settings;
    settings.codec = VideoCodec::VP8;
    settings.framerate = 30;
    settings.bitrate = 200'000;
    settings.content = VideoEncoder::Content::Camera;
    VideoEncoder encoder(settings);
    if (!encoder.hasEncoder()) SKIP("no supported VP8 encoder is available");

    AVFrame *input = av_frame_alloc();
    REQUIRE(input);
    input->format = encoder.softwareFormat();
    input->width = 64;
    input->height = 48;
    REQUIRE(av_frame_get_buffer(input, 32) >= 0);
    REQUIRE(av_frame_make_writable(input) >= 0);
    for (int plane = 0; plane < 3; ++plane) {
        const int rows = input->height / (plane == 0 ? 1 : 2);
        const int columns = input->width / (plane == 0 ? 1 : 2);
        for (int row = 0; row < rows; ++row)
            std::fill_n(input->data[plane] + row * input->linesize[plane], columns, plane == 0 ? 64 : 128);
    }

    std::vector<EncodedVideoFrame> encoded;
    constexpr uint32_t timestamp = 90'000;
    REQUIRE(encoder.encode(input, timestamp, encoded) == VideoEncoder::Outcome::Encoded);
    av_frame_free(&input);
    REQUIRE_FALSE(encoded.empty());
    REQUIRE(encoded.front().keyframe);
    REQUIRE(encoded.front().rtpTimestamp == timestamp);

    auto result = std::make_shared<PresentedFrame>();
    VideoDecoder decoder(false, 1);
    decoder.setSink(std::make_shared<CollectingSink>(result));
    for (const auto &frame : encoded)
        decoder.push(VideoCodec::VP8, frame);

    std::unique_lock lock(result->mutex);
    REQUIRE(result->delivered.wait_for(lock, std::chrono::seconds(2), [&] { return result->frames > 0; }));
    REQUIRE(result->width == 64);
    REQUIRE(result->height == 48);
    REQUIRE(result->timestamp == timestamp);
    REQUIRE(result->luma == Catch::Approx(64).margin(12));
}
