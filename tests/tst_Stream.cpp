#include "Core/Stream/StreamDecoder.hpp"
#include "Core/Stream/StreamQuality.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

using namespace Styx;

TEST_CASE("Source stream quality retains native dimensions without odd H264 sizes", "[stream][quality]")
{
    auto quality = Core::Stream::StreamQuality::byId("source15");
    quality.captureCursor = false;
    const auto resolved = quality.resolved({3441, 1441});
    CHECK(resolved.size == Platform::Size{3440, 1440});
    CHECK(resolved.framerate == 15);
    CHECK_FALSE(resolved.captureCursor);
    CHECK(resolved.bitrate <= resolved.maxBitrate);

    const auto small = quality.resolved({640, 480});
    CHECK(small.size == Platform::Size{640, 480});
    const auto large = quality.resolved({7680, 4320});
    CHECK(large.bitrate == 14'000'000);
    CHECK(large.maxBitrate == 16'000'000);
}

TEST_CASE("Gaming quality does not change when the source is a larger monitor", "[stream][quality]")
{
    const auto quality = Core::Stream::StreamQuality::byId("1440p60").resolved({3840, 2160});
    CHECK(quality.size == Platform::Size{2560, 1440});
    CHECK(quality.framerate == 60);
    CHECK(quality.needsNitro);
}

namespace {
struct Packet {
    std::string bytes;
    bool keyframe;
};

std::vector<Packet> encodeClip()
{
    const auto *codec = avcodec_find_encoder_by_name("libopenh264");
    if (!codec) codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) return {};
    auto *encoder = avcodec_alloc_context3(codec);
    encoder->width = 320;
    encoder->height = 240;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = {1, 30};
    encoder->framerate = {30, 1};
    encoder->bit_rate = 400000;
    encoder->gop_size = 30;
    encoder->max_b_frames = 0;
    encoder->thread_count = 1;
    av_opt_set(encoder->priv_data, "preset", "ultrafast", 0);
    av_opt_set(encoder->priv_data, "tune", "zerolatency", 0);
    if (avcodec_open2(encoder, codec, nullptr) < 0) {
        avcodec_free_context(&encoder);
        return {};
    }
    auto *frame = av_frame_alloc();
    frame->width = encoder->width;
    frame->height = encoder->height;
    frame->format = encoder->pix_fmt;
    REQUIRE(av_frame_get_buffer(frame, 32) == 0);
    auto *packet = av_packet_alloc();
    std::vector<Packet> result;
    auto drain = [&] {
        while (avcodec_receive_packet(encoder, packet) >= 0) {
            result.push_back({std::string(reinterpret_cast<const char *>(packet->data), packet->size),
                              bool(packet->flags & AV_PKT_FLAG_KEY)});
            av_packet_unref(packet);
        }
    };
    for (int index = 0; index < 6; ++index) {
        REQUIRE(av_frame_make_writable(frame) == 0);
        for (int y = 0; y < frame->height; ++y)
            std::memset(frame->data[0] + y * frame->linesize[0], 80 + index * 3, frame->width);
        for (int y = 0; y < frame->height / 2; ++y) {
            std::memset(frame->data[1] + y * frame->linesize[1], 90, frame->width / 2);
            std::memset(frame->data[2] + y * frame->linesize[2], 240, frame->width / 2);
        }
        frame->pts = index;
        REQUIRE(avcodec_send_frame(encoder, frame) == 0);
        drain();
    }
    REQUIRE(avcodec_send_frame(encoder, nullptr) == 0);
    drain();
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    return result;
}
}

TEST_CASE("Live H264 decoding presents each picture without frame-thread delay", "[stream][decode]")
{
    const auto clip = encodeClip();
    if (clip.empty()) SKIP("FFmpeg has no software H264 encoder");
    REQUIRE(clip.size() == 6);
    REQUIRE(clip.front().keyframe);
    Core::Stream::StreamDecoder decoder;
    decoder.setTargetSize({160, 90});
    int presented = 0;
    Platform::ImageBuffer last;
    decoder.onFrameDecoded = [&](Platform::ImageBuffer image, Platform::Size source) {
        CHECK(source == Platform::Size{320, 240});
        last = std::move(image);
        ++presented;
    };
    decoder.decode(clip[1].bytes, 3000, false);
    CHECK(presented == 0);
    for (size_t index = 0; index < clip.size(); ++index) {
        decoder.decode(clip[index].bytes, uint32_t(index * 3000), clip[index].keyframe);
        CHECK(presented == int(index + 1));
    }
    REQUIRE(last.width == 120);
    REQUIRE(last.height == 90);
    const size_t centre = (size_t(last.height / 2) * last.width + last.width / 2) * 4;
    CHECK(int(last.pixels[centre]) > int(last.pixels[centre + 1]) + 50);
    CHECK(int(last.pixels[centre]) > int(last.pixels[centre + 2]) + 50);
}
