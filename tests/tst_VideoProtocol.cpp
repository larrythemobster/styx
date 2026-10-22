// Behavioral coverage for the Qt-free port of upstream/video-streams RTP/RTCP code.
#include <catch2/catch_test_macros.hpp>

#include "Discord/Voice/Rtcp.hpp"
#include "Discord/Voice/SendSideEstimator.hpp"
#include "Discord/Voice/TransportFeedback.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Discord/Voice/VideoPacketizer.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace Styx::Discord::Voice;
namespace {
using Bytes = std::vector<uint8_t>;

void appendNal(Bytes &frame, std::initializer_list<uint8_t> header, size_t body, bool longStart = false)
{
    frame.insert(frame.end(), longStart ? std::initializer_list<uint8_t>{0, 0, 0, 1} : std::initializer_list<uint8_t>{0, 0, 1});
    frame.insert(frame.end(), header);
    for (size_t i = 0; i < body; ++i)
        frame.push_back(static_cast<uint8_t>(1 + (i % 251)));
}
Bytes h264()
{
    Bytes frame; appendNal(frame, {0x67}, 9, true); appendNal(frame, {0x68}, 5, true); appendNal(frame, {0x65}, 4000, true); frame[frame.size() - 4000] |= 0x80; return frame;
}
Bytes h265()
{
    Bytes frame; appendNal(frame, {0x40, 0x01}, 9, true); appendNal(frame, {0x42, 0x01}, 5, true); appendNal(frame, {0x26, 0x01}, 4000, true); frame[frame.size() - 4000] |= 0x80; return frame;
}
void leb(Bytes &out, size_t value) { do { uint8_t byte = value & 0x7f; value >>= 7; out.push_back(value ? byte | 0x80 : byte); } while (value); }
Bytes av1()
{
    Bytes frame = {0x0a}; leb(frame, 3); frame.insert(frame.end(), {1, 2, 3});
    frame.push_back(0x32); leb(frame, 2500); for (int i = 0; i < 2500; ++i) frame.push_back(static_cast<uint8_t>(i));
    return frame;
}
Bytes rebuild(VideoCodec codec, const std::vector<Bytes> &payloads)
{
    std::vector<std::span<const uint8_t>> views; for (const auto &payload : payloads) views.emplace_back(payload);
    Bytes frame; REQUIRE(assembleFrame(codec, views, frame)); return frame;
}
}

TEST_CASE("Video packetizers preserve RTP codec payload layouts", "[video][rtp]")
{
    for (const auto &[codec, frame] : std::array<std::pair<VideoCodec, Bytes>, 4>{{{VideoCodec::H264, h264()}, {VideoCodec::H265, h265()}, {VideoCodec::VP8, Bytes(4000, 0x42)}, {VideoCodec::AV1, av1()}}}) {
        VideoPacketizer packetizer(codec, 0x7ffe);
        auto payloads = packetizer.packetize(frame, true, 300);
        REQUIRE(payloads.size() > 1);
        for (const Bytes &payload : payloads)
            REQUIRE(payload.size() <= 300);
        Bytes assembled = rebuild(codec, payloads);
        if (codec == VideoCodec::AV1) { prepareForDecoder(codec, assembled); REQUIRE(assembled == frame); }
        else REQUIRE(assembled == frame);
        REQUIRE(startsKeyframe(codec, payloads.front()));
    }
}

TEST_CASE("Video packetizers reject malformed frames and honor boundaries", "[video][malformed]")
{
    VideoPacketizer h264Packetizer(VideoCodec::H264);
    REQUIRE(h264Packetizer.packetize({std::array<uint8_t, 2>{0, 1}}, false, 300).empty());
    REQUIRE(h264Packetizer.packetize(h264(), false, VideoPacketizer::MIN_PAYLOAD_SIZE - 1).empty());
    const std::array<uint8_t, 1> truncatedFu = {28};
    const std::array<uint8_t, 1> truncatedVp8 = {0x90};
    const std::array<uint8_t, 1> truncatedAv1 = {0x80};
    const std::array<std::span<const uint8_t>, 1> badH264 = {truncatedFu};
    const std::array<std::span<const uint8_t>, 1> badVp8 = {truncatedVp8};
    const std::array<std::span<const uint8_t>, 1> badAv1 = {truncatedAv1};
    Bytes output;
    REQUIRE_FALSE(assembleFrame(VideoCodec::H264, badH264, output));
    REQUIRE_FALSE(assembleFrame(VideoCodec::VP8, badVp8, output));
    REQUIRE_FALSE(assembleFrame(VideoCodec::AV1, badAv1, output));
}

TEST_CASE("RTCP writers and parser retain NACK PLI reports", "[rtcp]")
{
    std::array<uint8_t, 128> packet{};
    const std::array<uint16_t, 4> requested = {65534, 65535, 0, 3};
    const int nackSize = writeNack(packet, 1, 2, requested); REQUIRE(nackSize > 0);
    auto received = parseRtcp(std::span(packet).first(nackSize)); REQUIRE(received.nacks.size() == 1); REQUIRE(received.nacks[0].sequences == std::vector<uint16_t>(requested.begin(), requested.end()));
    const int pliSize = writePli(packet, 1, 7); REQUIRE(pliSize == 12);
    received = parseRtcp(std::span(packet).first(pliSize)); REQUIRE(received.keyframeRequests == std::vector<uint32_t>{7});
    ReceptionReport report{.ssrc = 9, .fractionLost = 2, .cumulativeLost = -3, .highestSequence = 8};
    const int rrSize = writeReceiverReport(packet, 1, std::span(&report, 1)); REQUIRE(rrSize == 32);
    received = parseRtcp(std::span(packet).first(rrSize)); REQUIRE(received.receptionReports[0].cumulativeLost == -3);
}

TEST_CASE("Transport feedback handles loss and sequence wrap", "[rtcp][feedback]")
{
    TransportFeedback feedback; feedback.onPacket(65534, 1'000'000); feedback.onPacket(0, 1'001'000); feedback.onPacket(1, 1'001'250);
    std::array<uint8_t, 256> wire{}; const int size = feedback.write(wire, 1, 2); REQUIRE(size > 0);
    const auto report = parseRtcp(std::span(wire).first(size)).transportReports.at(0);
    REQUIRE(report.baseSequence == 65534); REQUIRE(report.packets.size() == 4); REQUIRE_FALSE(report.packets[1].received);
}

TEST_CASE("Send-side estimator accepts transport feedback", "[estimator]")
{
    SendSideEstimator estimator; estimator.restart(65534);
    for (int i = 0; i < 8; ++i) estimator.numberPacket(1'000'000 + i * 2'000, 1200);
    ReceivedRtcp::TransportReport report; report.baseSequence = 65534; report.referenceTime = 15;
    report.packets.assign(8, {.received = true, .delta = 8});
    REQUIRE_FALSE(estimator.onReport(report, 1'100'000));
    REQUIRE(estimator.reportsTaken() == 1); REQUIRE(estimator.acknowledged() >= 0);
}

TEST_CASE("Send-side estimator detects persistent queue growth", "[estimator][feedback]")
{
    SendSideEstimator estimator;
    estimator.restart(1);
    constexpr int packets = 100;
    for (int i = 0; i < packets; ++i)
        estimator.numberPacket(1'000'000 + i * 6'000, 1200);
    ReceivedRtcp::TransportReport report;
    report.baseSequence = 1;
    report.referenceTime = 15;
    report.packets.assign(packets, {.received = true, .delta = 48}); // 12 ms arrivals after 6 ms sends.
    estimator.onReport(report, 2'000'000);
    REQUIRE(estimator.acknowledged() > 0);
    REQUIRE(estimator.estimate() > 0);
    REQUIRE(estimator.overusing());
}
