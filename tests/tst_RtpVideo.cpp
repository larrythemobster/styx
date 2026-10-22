// Tests for receiving video over RTP: the depacketizers, the per-SSRC receiver and the RTCP it
// sends. Packets for the depacketizers come from ffmpeg's RTP muxer, so the reference is not
// this code's own idea of the formats; AV1 is also packetized by hand, because the muxer only
// learnt it recently. The end-to-end encryption cases run frames through libdave's encryptor,
// the same packetizing, and libdave's decryptor.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "Discord/Voice/ProtocolEndian.hpp"
#include "Discord/Voice/Rtcp.hpp"
#include "Discord/Voice/TransportFeedback.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Discord/Voice/VideoPacketizer.hpp"
#include "Discord/Voice/VideoReceiver.hpp"
#include "VideoRtpSupport.hpp"

#include <bytes/bytes.h>
#include <dave/dave_interfaces.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Styx::Discord::Voice;
using namespace VideoRtpSupport;

namespace {

constexpr uint32_t SSRC = 0x1234;

// Bytes that can never form a start code, so a NAL payload made of them needs no escaping.
Bytes filler(size_t size, uint32_t seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> byte(1, 255);
    Bytes bytes(size);
    for (uint8_t &value : bytes)
        value = static_cast<uint8_t>(byte(generator));
    return bytes;
}

void append(Bytes &out, const Bytes &bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendNal(Bytes &frame, std::initializer_list<uint8_t> header, size_t payloadSize, uint32_t seed, int firstByteOr = 0)
{
    frame.insert(frame.end(), { 0, 0, 0, 1 });
    frame.insert(frame.end(), header);
    Bytes payload = filler(payloadSize, seed);
    payload[0] |= static_cast<uint8_t>(firstByteOr);
    append(frame, payload);
}

Bytes h264Keyframe()
{
    Bytes frame;
    appendNal(frame, { 0x67 }, 20, 1);
    appendNal(frame, { 0x68 }, 6, 2);
    appendNal(frame, { 0x65 }, 5000, 3, 0x80);
    return frame;
}

Bytes h264DeltaFrame()
{
    Bytes frame;
    appendNal(frame, { 0x06 }, 12, 4);
    appendNal(frame, { 0x41 }, 3000, 5, 0x80);
    return frame;
}

Bytes h265Keyframe()
{
    Bytes frame;
    appendNal(frame, { 0x40, 0x01 }, 22, 6);
    appendNal(frame, { 0x42, 0x01 }, 30, 7);
    appendNal(frame, { 0x44, 0x01 }, 8, 8);
    appendNal(frame, { 0x26, 0x01 }, 5000, 9, 0x80);
    return frame;
}

Bytes h265DeltaFrame()
{
    Bytes frame;
    appendNal(frame, { 0x02, 0x01 }, 2500, 10, 0x80);
    return frame;
}

Bytes vp8Frame(bool keyframe, size_t size, uint32_t seed)
{
    Bytes frame = filler(size, seed);
    frame[0] = static_cast<uint8_t>(keyframe ? (frame[0] & ~1) : (frame[0] | 1));
    return frame;
}

void appendObu(Bytes &frame, uint8_t type, const Bytes &payload, bool withSize = true)
{
    frame.push_back(static_cast<uint8_t>((type << 3) | (withSize ? 0x02 : 0)));
    if (withSize)
        appendLeb128(frame, payload.size());
    append(frame, payload);
}

constexpr uint8_t OBU_SEQUENCE_HEADER = 1;
constexpr uint8_t OBU_TEMPORAL_DELIMITER = 2;
constexpr uint8_t OBU_FRAME = 6;

// One AV1 temporal unit as an encoder writes it, and the same without the temporal delimiter,
// which neither the RTP format nor the frame encryptor carries.
Bytes av1Frame(bool withDelimiter)
{
    Bytes frame;
    if (withDelimiter)
        appendObu(frame, OBU_TEMPORAL_DELIMITER, {});
    appendObu(frame, OBU_SEQUENCE_HEADER, filler(11, 11));
    appendObu(frame, OBU_FRAME, filler(4000, 12));
    return frame;
}

bool reassemble(VideoCodec codec, const RtpPayloads &packets, Bytes &frame)
{
    std::vector<std::span<const uint8_t>> spans;
    for (const Bytes &payload : packets.payloads)
        spans.emplace_back(payload);
    return assembleFrame(codec, spans, frame);
}

class FixedKeyRatchet : public discord::dave::IKeyRatchet
{
public:
    discord::dave::EncryptionKey GetKey(discord::dave::KeyGeneration generation) noexcept override
    {
        std::vector<uint8_t> key(16, 0x5A);
        key[0] = static_cast<uint8_t>(generation);
        return discord::dave::EncryptionKey(std::move(key));
    }
    void DeleteKey(discord::dave::KeyGeneration) noexcept override { }
};

Bytes encryptFrame(discord::dave::Codec codec, const Bytes &frame)
{
    auto encryptor = discord::dave::CreateEncryptor();
    encryptor->SetKeyRatchet(std::make_unique<FixedKeyRatchet>());
    encryptor->AssignSsrcToCodec(SSRC, codec);

    Bytes encrypted(encryptor->GetMaxCiphertextByteSize(discord::dave::MediaType::Video, frame.size()));
    size_t written = 0;
    const auto result = encryptor->Encrypt(discord::dave::MediaType::Video, SSRC, discord::dave::ArrayView<const uint8_t>(frame.data(), frame.size()), discord::dave::ArrayView<uint8_t>(encrypted.data(), encrypted.size()), &written);
    if (result != discord::dave::IEncryptor::Success)
        return {};
    encrypted.resize(written);
    return encrypted;
}

bool decryptFrame(const Bytes &encrypted, Bytes &frame)
{
    auto decryptor = discord::dave::CreateDecryptor();
    decryptor->TransitionToKeyRatchet(std::make_unique<FixedKeyRatchet>());

    frame.resize(decryptor->GetMaxPlaintextByteSize(discord::dave::MediaType::Video, encrypted.size()));
    size_t written = 0;
    const auto result = decryptor->Decrypt(discord::dave::MediaType::Video, discord::dave::ArrayView<const uint8_t>(encrypted.data(), encrypted.size()), discord::dave::ArrayView<uint8_t>(frame.data(), frame.size()), &written);
    frame.resize(written);
    return result == discord::dave::IDecryptor::Success;
}

// A receiver fed hand-made VP8 packets: one frame is `packetsPerFrame` packets.
struct Feed
{
    PacketPool pool;
    VideoReceiver receiver{ SSRC, VideoCodec::VP8 };
    int64_t now = 0;

    static Bytes payload(bool first, bool keyframe, uint8_t tag)
    {
        Bytes bytes = { static_cast<uint8_t>(first ? 0x10 : 0x00), static_cast<uint8_t>(keyframe ? 0x00 : 0x01), tag, tag };
        return bytes;
    }

    void send(uint16_t sequence, uint32_t timestamp, bool first, bool last, bool keyframe, bool retransmission = false)
    {
        PacketPool::Handle packet = pool.acquire();
        const Bytes bytes = payload(first, keyframe, static_cast<uint8_t>(sequence));
        packet->append(bytes);
        receiver.insert(sequence, timestamp, last, std::move(packet), now, retransmission);
    }

    // Sends frame number `index` (three packets, sequence numbers from `base`), leaving out `skip`.
    void sendFrame(int index, uint16_t base, bool keyframe, int skip = -1)
    {
        for (int i = 0; i < 3; i++) {
            if (i != skip)
                send(static_cast<uint16_t>(base + index * 3 + i), 1000u * static_cast<uint32_t>(index), i == 0, i == 2, keyframe);
        }
    }
};

std::string toHex(const uint8_t *data, size_t size)
{
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i)
        ss << std::setw(2) << static_cast<int>(data[i]);
    return ss.str();
}

std::string toHex(const Bytes &bytes)
{
    return toHex(bytes.data(), bytes.size());
}

Bytes fromHex(std::string_view hex)
{
    Bytes out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto byteVal = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        out.push_back(static_cast<uint8_t>((byteVal(hex[i]) << 4) | byteVal(hex[i + 1])));
    }
    return out;
}

} // namespace

TEST_CASE("H264 against ffmpeg", "[video][rtp]")
{
    for (const bool keyframe : { true, false }) {
        const Bytes frame = keyframe ? h264Keyframe() : h264DeltaFrame();
        const RtpPayloads packets = packetizeWithFfmpeg(AV_CODEC_ID_H264, frame, keyframe);
        REQUIRE(packets.payloads.size() > 2);
        REQUIRE(packets.markers.back());
        REQUIRE(startsKeyframe(VideoCodec::H264, packets.payloads.front()) == keyframe);

        Bytes rebuilt;
        REQUIRE(reassemble(VideoCodec::H264, packets, rebuilt));
        REQUIRE(rebuilt == frame);
    }
}

TEST_CASE("H265 against ffmpeg", "[video][rtp]")
{
    for (const bool keyframe : { true, false }) {
        const Bytes frame = keyframe ? h265Keyframe() : h265DeltaFrame();
        const RtpPayloads packets = packetizeWithFfmpeg(AV_CODEC_ID_HEVC, frame, keyframe);
        REQUIRE(packets.payloads.size() > 2);
        REQUIRE(startsKeyframe(VideoCodec::H265, packets.payloads.front()) == keyframe);

        Bytes rebuilt;
        REQUIRE(reassemble(VideoCodec::H265, packets, rebuilt));
        REQUIRE(rebuilt == frame);
    }
}

TEST_CASE("VP8 against ffmpeg", "[video][rtp]")
{
    for (const bool keyframe : { true, false }) {
        const Bytes frame = vp8Frame(keyframe, 4000, 20);
        const RtpPayloads packets = packetizeWithFfmpeg(AV_CODEC_ID_VP8, frame, keyframe);
        REQUIRE(packets.payloads.size() > 2);
        REQUIRE(startsKeyframe(VideoCodec::VP8, packets.payloads.front()) == keyframe);
        REQUIRE(!startsKeyframe(VideoCodec::VP8, packets.payloads[1]));

        Bytes rebuilt;
        REQUIRE(reassemble(VideoCodec::VP8, packets, rebuilt));
        REQUIRE(rebuilt == frame);
    }
}

TEST_CASE("AV1 against ffmpeg", "[video][rtp]")
{
    const RtpPayloads packets = packetizeWithFfmpeg(AV_CODEC_ID_AV1, av1Frame(true), true);
    if (packets.payloads.empty())
        SKIP("this ffmpeg has no AV1 RTP packetizer");

    Bytes rebuilt;
    REQUIRE(reassemble(VideoCodec::AV1, packets, rebuilt));
    prepareForDecoder(VideoCodec::AV1, rebuilt);
    REQUIRE(rebuilt == av1Frame(false));
}

TEST_CASE("AV1 by hand", "[video][rtp]")
{
    const RtpPayloads packets = packetizeAv1ByHand(av1Frame(false), true);
    REQUIRE(packets.payloads.size() > 2);
    REQUIRE(startsKeyframe(VideoCodec::AV1, packets.payloads.front()));
    REQUIRE(!startsKeyframe(VideoCodec::AV1, packets.payloads[1]));
    REQUIRE(!startsKeyframe(VideoCodec::AV1, packetizeAv1ByHand(av1Frame(false), false).payloads.front()));

    // As assembled, the last OBU has no size field: that is the form the sender encrypted.
    Bytes expected;
    appendObu(expected, OBU_SEQUENCE_HEADER, filler(11, 11));
    appendObu(expected, OBU_FRAME, filler(4000, 12), false);

    Bytes rebuilt;
    REQUIRE(reassemble(VideoCodec::AV1, packets, rebuilt));
    REQUIRE(rebuilt == expected);
}

TEST_CASE("AV1 last OBU gets its size back", "[video][rtp]")
{
    Bytes frame;
    appendObu(frame, OBU_SEQUENCE_HEADER, filler(11, 11));
    appendObu(frame, OBU_FRAME, filler(300, 12), false);

    prepareForDecoder(VideoCodec::AV1, frame);

    Bytes expected;
    appendObu(expected, OBU_SEQUENCE_HEADER, filler(11, 11));
    appendObu(expected, OBU_FRAME, filler(300, 12));
    REQUIRE(frame == expected);

    // Already complete: left alone.
    prepareForDecoder(VideoCodec::AV1, frame);
    REQUIRE(frame == expected);
}

TEST_CASE("Malformed payloads are rejected", "[video][rtp]")
{
    Bytes frame;
    const auto assembleOne = [&frame](VideoCodec codec, const Bytes &payload) {
        const std::span<const uint8_t> span(payload);
        return assembleFrame(codec, { &span, 1 }, frame);
    };

    // A fragment with no beginning, and an aggregate whose length runs past the packet.
    REQUIRE(!assembleOne(VideoCodec::H264, { 0x7C, 0x05, 0xAA }));
    REQUIRE(!assembleOne(VideoCodec::H264, { 0x18, 0x00, 0x09, 0x67, 0x01 }));
    REQUIRE(!assembleOne(VideoCodec::H265, { 0x62, 0x01, 0x13, 0xAA }));
    REQUIRE(!assembleOne(VideoCodec::H265, { 0x60, 0x01, 0x00, 0x09, 0x40, 0x01 }));
    // A VP8 frame that does not open with the start of its first partition.
    REQUIRE(!assembleOne(VideoCodec::VP8, { 0x00, 0x01, 0x02 }));
    // An AV1 packet continuing an OBU nobody started, and one promising a continuation.
    REQUIRE(!assembleOne(VideoCodec::AV1, { 0x80, 0x02, 0x30, 0x01 }));
    REQUIRE(!assembleOne(VideoCodec::AV1, { 0x40, 0x02, 0x30, 0x01 }));
    REQUIRE(!assembleOne(VideoCodec::AV1, { 0x00, 0x09, 0x30, 0x01 }));
    REQUIRE(!assembleOne(VideoCodec::Unknown, { 0x01, 0x02 }));
}

TEST_CASE("Encrypted frames survive the receive chain", "[video][rtp][dave]")
{
    const auto videoCodec = GENERATE(VideoCodec::H264, VideoCodec::H265, VideoCodec::VP8, VideoCodec::AV1);

    Bytes original;
    Bytes expected;
    discord::dave::Codec daveCodec = discord::dave::Codec::Unknown;
    AVCodecID ffmpegCodec = AV_CODEC_ID_NONE;
    switch (videoCodec) {
    case VideoCodec::H264:
        original = expected = h264Keyframe();
        daveCodec = discord::dave::Codec::H264;
        ffmpegCodec = AV_CODEC_ID_H264;
        break;
    case VideoCodec::H265:
        original = expected = h265Keyframe();
        daveCodec = discord::dave::Codec::H265;
        ffmpegCodec = AV_CODEC_ID_HEVC;
        break;
    case VideoCodec::VP8:
        original = expected = vp8Frame(true, 4000, 20);
        daveCodec = discord::dave::Codec::VP8;
        ffmpegCodec = AV_CODEC_ID_VP8;
        break;
    case VideoCodec::AV1:
        original = av1Frame(true);
        expected = av1Frame(false);
        daveCodec = discord::dave::Codec::AV1;
        break;
    case VideoCodec::Unknown:
        break;
    }

    const Bytes encrypted = encryptFrame(daveCodec, original);
    REQUIRE(!encrypted.empty());
    REQUIRE(encrypted != original);

    const RtpPayloads packets = videoCodec == VideoCodec::AV1 ? packetizeAv1ByHand(encrypted, true) : packetizeWithFfmpeg(ffmpegCodec, encrypted, true);
    REQUIRE(packets.payloads.size() > 2);

    Bytes received;
    REQUIRE(reassemble(videoCodec, packets, received));
    REQUIRE(received == encrypted);

    Bytes decrypted;
    REQUIRE(decryptFrame(received, decrypted));
    prepareForDecoder(videoCodec, decrypted);
    REQUIRE(decrypted == expected);
}

TEST_CASE("Keyframe keeps what precedes its parameter sets", "[video][rtp]")
{
    // An encoder may open the access unit with a delimiter; the frame starts there, not at
    // the parameter sets that mark it as a key frame.
    const std::vector<Bytes> packets = {
        { 0x41, 0x80, 0x11 },
        { 0x09, 0xF0 },
        { 0x18, 0x00, 0x03, 0x67, 0x42, 0x00, 0x00, 0x02, 0x68, 0xCE },
        { 0x65, 0x88, 0x84 },
    };
    const std::vector<uint32_t> timestamps = { 3000, 6000, 6000, 6000 };

    PacketPool pool;
    VideoReceiver receiver(SSRC, VideoCodec::H264);
    for (size_t i = 0; i < packets.size(); i++) {
        PacketPool::Handle packet = pool.acquire();
        packet->append(packets[i]);
        receiver.insert(static_cast<uint16_t>(500 + i), timestamps[i], i == 0 || i + 1 == packets.size(), std::move(packet), 0);
    }

    const std::vector<EncodedVideoFrame> frames = receiver.takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].keyframe);
    const Bytes expected = { 0, 0, 0, 1, 0x09, 0xF0, 0, 0, 0, 1, 0x67, 0x42, 0x00, 0, 0, 0, 1, 0x68, 0xCE, 0, 0, 0, 1, 0x65, 0x88, 0x84 };
    REQUIRE(frames[0].data == expected);

    // The same holds when the parameter sets are not first inside an aggregation packet.
    REQUIRE(startsKeyframe(VideoCodec::H264, Bytes{ 0x18, 0x00, 0x02, 0x09, 0xF0, 0x00, 0x03, 0x67, 0x42, 0x00 }));
    REQUIRE(!startsKeyframe(VideoCodec::H264, Bytes{ 0x18, 0x00, 0x02, 0x09, 0xF0, 0x00, 0x03, 0x41, 0x80, 0x00 }));
    REQUIRE(startsKeyframe(VideoCodec::H265, Bytes{ 0x60, 0x01, 0x00, 0x03, 0x46, 0x01, 0x50, 0x00, 0x04, 0x40, 0x01, 0x0C, 0x01 }));
}

TEST_CASE("Picture without its parameter sets is not a start", "[video][rtp]")
{
    const Bytes parameterSets = { 0x18, 0x00, 0x03, 0x67, 0x42, 0x00, 0x00, 0x02, 0x68, 0xCE };
    const Bytes picture = { 0x65, 0x88, 0x84 };
    const Bytes delta = { 0x41, 0x80, 0x11 };
    const auto send = [](VideoReceiver &receiver, PacketPool &pool, uint16_t sequence, uint32_t timestamp, bool marker, const Bytes &bytes) {
        PacketPool::Handle packet = pool.acquire();
        packet->append(bytes);
        receiver.insert(sequence, timestamp, marker, std::move(packet), 0);
    };

    // Subscribed in the middle of a key frame: its picture arrives, its parameter sets went
    // by before. The frame would be incomplete, so it is not taken.
    PacketPool pool;
    VideoReceiver late(SSRC, VideoCodec::H264);
    send(late, pool, 501, 6000, true, picture);
    REQUIRE(late.takeFrames().empty());
    REQUIRE(late.poll(0).keyframeRequest);

    // The whole of the next key frame is.
    send(late, pool, 510, 9000, false, parameterSets);
    send(late, pool, 511, 9000, true, picture);
    REQUIRE(late.takeFrames().size() == 1);

    // A sender that puts no parameter sets in its key frames is still followed, because the
    // packet before the picture shows where the frame begins.
    VideoReceiver bare(SSRC, VideoCodec::H264);
    send(bare, pool, 700, 3000, true, delta);
    send(bare, pool, 701, 6000, true, picture);
    const std::vector<EncodedVideoFrame> frames = bare.takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].keyframe);
}

TEST_CASE("A keyframe is asked for five times a second while packets come", "[video][rtp]")
{
    // The official client's numbers.
    REQUIRE(VideoReceiver::KEYFRAME_REQUEST_INTERVAL_MS == 200);
    REQUIRE(VideoReceiver::SILENT_AFTER_MS == 5000);

    // Nothing has come yet, so there is nobody to ask anything of.
    Feed feed;
    REQUIRE(!feed.receiver.poll(0).keyframeRequest);

    feed.now = 1000;
    feed.sendFrame(0, 100, false);
    int requests = 0;
    for (int64_t now = 1000; now < 2000; now += 10)
        requests += feed.receiver.poll(now).keyframeRequest ? 1 : 0;
    REQUIRE(requests == 5);

    // The stream goes quiet: asked up to five seconds after its last packet and no longer.
    REQUIRE(feed.receiver.poll(1000 + VideoReceiver::SILENT_AFTER_MS - 1).keyframeRequest);
    for (int64_t now = 1000 + VideoReceiver::SILENT_AFTER_MS; now < 20000; now += 100)
        REQUIRE(!feed.receiver.poll(now).keyframeRequest);

    // Its next packet, of whatever kind, has it asked again at once.
    feed.now = 20000;
    feed.sendFrame(1, 100, false);
    REQUIRE(feed.receiver.poll(20000).keyframeRequest);
    REQUIRE(feed.receiver.takeFrames().empty());
}

TEST_CASE("Nothing is delivered before a keyframe", "[video][rtp]")
{
    Feed feed;
    feed.sendFrame(0, 100, false);
    feed.sendFrame(1, 100, false);
    REQUIRE(feed.receiver.takeFrames().empty());

    VideoReceiver::Feedback feedback = feed.receiver.poll(feed.now);
    REQUIRE(feedback.keyframeRequest);
    REQUIRE(feedback.nacks.empty());

    // Asked once, then not again until the interval has passed.
    REQUIRE(!feed.receiver.poll(feed.now + 20).keyframeRequest);
    REQUIRE(feed.receiver.poll(feed.now + VideoReceiver::KEYFRAME_REQUEST_INTERVAL_MS).keyframeRequest);

    feed.sendFrame(2, 100, true);
    const std::vector<EncodedVideoFrame> frames = feed.receiver.takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].keyframe);
    REQUIRE(frames[0].rtpTimestamp == 2000u);
    REQUIRE(!feed.receiver.poll(feed.now + 5000).keyframeRequest);
}

TEST_CASE("Frames come out in order", "[video][rtp]")
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    feed.sendFrame(1, 100, false);
    feed.sendFrame(2, 100, false);

    const std::vector<EncodedVideoFrame> frames = feed.receiver.takeFrames();
    REQUIRE(frames.size() == 3);
    REQUIRE(frames[0].keyframe);
    REQUIRE(!frames[1].keyframe);
    REQUIRE(frames[1].rtpTimestamp == 1000u);
    REQUIRE(frames[2].rtpTimestamp == 2000u);
    // The payload descriptor is gone and the three packets' bytes follow each other.
    REQUIRE(frames[0].data.size() == 9);
    REQUIRE(frames[0].data[1] == uint8_t(100));
    REQUIRE(frames[0].data[4] == uint8_t(101));
    REQUIRE(feed.receiver.takeFrames().empty());
}

TEST_CASE("Reordered packets are held", "[video][rtp]")
{
    Feed feed;
    feed.send(102, 0, false, true, true);
    feed.send(101, 0, false, false, true);
    REQUIRE(feed.receiver.takeFrames().empty());
    feed.send(100, 0, true, false, true);

    // The next frame arrives before the first one's packets do, and waits for them.
    Feed late;
    late.sendFrame(0, 100, true);
    REQUIRE(late.receiver.takeFrames().size() == 1);
    late.sendFrame(2, 100, false);
    REQUIRE(late.receiver.takeFrames().empty());
    late.sendFrame(1, 100, false);
    REQUIRE(late.receiver.takeFrames().size() == 2);

    REQUIRE(feed.receiver.takeFrames().size() == 1);
}

TEST_CASE("Lost packet is requested and accepted", "[video][rtp]")
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    feed.sendFrame(1, 100, false, 1);
    feed.sendFrame(2, 100, false);
    REQUIRE(feed.receiver.takeFrames().size() == 1);

    // Not asked for at once: it may only be out of order.
    REQUIRE(feed.receiver.poll(feed.now).nacks.empty());

    feed.now += VideoReceiver::REORDER_GRACE_MS;
    VideoReceiver::Feedback feedback = feed.receiver.poll(feed.now);
    REQUIRE(feedback.nacks == std::vector<uint16_t>{ 104 });
    REQUIRE(!feedback.keyframeRequest);

    // Asked again only after the retry interval.
    REQUIRE(feed.receiver.poll(feed.now + 20).nacks.empty());
    feed.now += VideoReceiver::NACK_RETRY_MS;
    REQUIRE(feed.receiver.poll(feed.now).nacks == std::vector<uint16_t>{ 104 });

    feed.send(104, 1000, false, false, false, true);
    REQUIRE(feed.receiver.takeFrames().size() == 2);
    REQUIRE(feed.receiver.stats().retransmitted == uint64_t(1));
    REQUIRE(feed.receiver.poll(feed.now + 5000).nacks.empty());
}

TEST_CASE("Unrepaired loss waits for a keyframe", "[video][rtp]")
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    feed.sendFrame(1, 100, false, 0);
    feed.sendFrame(2, 100, false);
    REQUIRE(feed.receiver.takeFrames().size() == 1);

    feed.now += VideoReceiver::NACK_GIVE_UP_MS + 1;
    const VideoReceiver::Feedback feedback = feed.receiver.poll(feed.now);
    REQUIRE(feedback.keyframeRequest);
    REQUIRE(feedback.nacks.empty());

    // Frames that build on the lost one are of no use, even complete.
    feed.sendFrame(3, 100, false);
    REQUIRE(feed.receiver.takeFrames().empty());

    feed.sendFrame(4, 100, true);
    feed.sendFrame(5, 100, false);
    const std::vector<EncodedVideoFrame> frames = feed.receiver.takeFrames();
    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0].keyframe);
    REQUIRE(frames[0].rtpTimestamp == 4000u);
}

TEST_CASE("Sequence numbers wrap", "[video][rtp]")
{
    Feed feed;
    const uint16_t base = 65530;
    for (int index = 0; index < 5; index++)
        feed.sendFrame(index, base, index == 0);
    REQUIRE(feed.receiver.takeFrames().size() == 5);

    // A loss just past the wrap is named by its 16-bit number.
    feed.sendFrame(5, base, false, 1);
    feed.sendFrame(6, base, false);
    feed.now += VideoReceiver::REORDER_GRACE_MS;
    REQUIRE(feed.receiver.poll(feed.now).nacks == std::vector<uint16_t>{ static_cast<uint16_t>(base + 16) });
}

TEST_CASE("A stray packet does not restart the stream", "[video][rtp]")
{
    Feed feed;
    feed.sendFrame(0, 20000, true);
    feed.sendFrame(1, 20000, false);
    REQUIRE(feed.receiver.takeFrames().size() == 2);

    // A long-delayed duplicate from far behind.
    feed.send(100, 0, true, true, false);
    REQUIRE(feed.receiver.stats().strays == uint64_t(1));
    REQUIRE(feed.receiver.stats().resyncs == uint64_t(0));

    feed.sendFrame(2, 20000, false);
    REQUIRE(feed.receiver.takeFrames().size() == 1);
    REQUIRE(!feed.receiver.poll(feed.now).keyframeRequest);
}

TEST_CASE("A sender that restarts its numbering is followed", "[video][rtp]")
{
    Feed feed;
    feed.sendFrame(0, 20000, true);
    REQUIRE(feed.receiver.takeFrames().size() == 1);

    // The first packet of the new numbering could be a stray; the one after it shows it is not.
    feed.sendFrame(1, 100, false);
    REQUIRE(feed.receiver.stats().strays == uint64_t(1));
    REQUIRE(feed.receiver.stats().resyncs == uint64_t(1));
    REQUIRE(feed.receiver.takeFrames().empty());
    REQUIRE(feed.receiver.poll(feed.now).keyframeRequest);

    feed.sendFrame(2, 100, true);
    feed.sendFrame(3, 100, false);
    REQUIRE(feed.receiver.takeFrames().size() == 2);
}

TEST_CASE("Undecodable output asks for a keyframe", "[video][rtp]")
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    REQUIRE(feed.receiver.takeFrames().size() == 1);
    REQUIRE(!feed.receiver.poll(feed.now).keyframeRequest);

    feed.receiver.requestKeyframe();
    REQUIRE(feed.receiver.poll(feed.now).keyframeRequest);
    feed.sendFrame(1, 100, false);
    REQUIRE(feed.receiver.takeFrames().empty());
    feed.sendFrame(2, 100, true);
    REQUIRE(feed.receiver.takeFrames().size() == 1);

    // A stream that keeps failing is not allowed to ask more than once per interval.
    feed.receiver.requestKeyframe();
    REQUIRE(!feed.receiver.poll(feed.now + 20).keyframeRequest);
    REQUIRE(feed.receiver.poll(feed.now + VideoReceiver::KEYFRAME_REQUEST_INTERVAL_MS).keyframeRequest);
}

TEST_CASE("Reception report counts loss", "[video][rtp]")
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    feed.sendFrame(1, 100, false, 1);
    feed.sendFrame(2, 100, false);
    feed.sendFrame(3, 100, false, 2);

    ReceptionReport report = feed.receiver.receptionReport();
    REQUIRE(report.ssrc == SSRC);
    REQUIRE((report.highestSequence & 0xFFFFu) == 110u);
    REQUIRE(report.cumulativeLost == 1);
    // One of the eleven packets expected so far, as a fraction of 256.
    REQUIRE(int(report.fractionLost) == 256 / 11);

    // The packet missing from the end of that interval is counted in the next one.
    feed.sendFrame(4, 100, false);
    report = feed.receiver.receptionReport();
    REQUIRE(report.cumulativeLost == 2);
    REQUIRE(int(report.fractionLost) == 256 / 4);
}

TEST_CASE("Received RTCP is read", "[rtcp]")
{
    // A sender report as the server sends one: the NTP time is ee73cd84.7cbccc93.
    ReceivedRtcp received = parseRtcp(fromHex("80c800060000008aee73cd847cbccc930512806400000053000129f0"));
    REQUIRE(received.senderReports.size() == 1);
    REQUIRE(received.senderReports[0].ssrc == 0x8Au);
    REQUIRE(received.senderReports[0].compactNtp == 0xCD847CBCu);
    REQUIRE(!received.hasInboundEstimate);

    // A receiver report with Discord's estimate behind it: mantissa 0x22560 under exponent 6.
    received = parseRtcp(fromHex("81c90007000003b1000003b100000000000000000000003b0000000000000000" "81cc00050000008a3153494401020304050607081a256000"));
    REQUIRE(received.senderReports.empty());
    REQUIRE(received.hasInboundEstimate);
    REQUIRE(received.inboundBitsPerSecond == uint64_t(9000960));
    REQUIRE(received.inboundLoss == 0.0);

    // Exponent 7, and a quarter of the packets lost.
    received = parseRtcp(fromHex("81cc00050000008a3153494401020304050607081efaf040"));
    REQUIRE(received.inboundBitsPerSecond == uint64_t(24999936));
    REQUIRE(received.inboundLoss == 0.25);

    // Another application packet, a cut-off one, and one that claims more than there is.
    REQUIRE(!parseRtcp(fromHex("81cc00050000008a5853494401020304050607081efaf040")).hasInboundEstimate);
    REQUIRE(!parseRtcp(fromHex("81cc00050000008a31534944010203")).hasInboundEstimate);
    REQUIRE(parseRtcp(fromHex("80c800ff0000008aee73cd847cbccc93")).senderReports.empty());
    REQUIRE(parseRtcp({}).senderReports.empty());
}

TEST_CASE("Reports on what this client sends are read", "[rtcp]")
{
    ReceptionReport sent;
    sent.ssrc = 7713;
    sent.fractionLost = 64;
    sent.cumulativeLost = -3;
    sent.highestSequence = 0x00010457;
    sent.jitter = 12;

    uint8_t packet[64];
    const ReceptionReport reports[] = { sent };
    const int size = writeReceiverReport(packet, 99, reports);
    REQUIRE(size > 0);

    const ReceivedRtcp received = parseRtcp({ packet, static_cast<size_t>(size) });
    REQUIRE(received.receptionReports.size() == 1);
    REQUIRE(received.receptionReports[0].ssrc == sent.ssrc);
    REQUIRE(received.receptionReports[0].fractionLost == sent.fractionLost);
    REQUIRE(received.receptionReports[0].cumulativeLost == sent.cumulativeLost);
    REQUIRE(received.receptionReports[0].highestSequence == sent.highestSequence);
    REQUIRE(received.receptionReports[0].jitter == sent.jitter);
}

TEST_CASE("Receiver report bytes", "[rtcp]")
{
    uint8_t buffer[64];

    REQUIRE(writeReceiverReport(buffer, 0x01020304, {}) == 8);
    REQUIRE(toHex(buffer, 8) == "80c9000101020304");

    ReceptionReport report;
    report.ssrc = 0x0A0B0C0D;
    report.fractionLost = 0x40;
    report.cumulativeLost = 0x010203;
    report.highestSequence = 0x00011234;
    report.jitter = 0x55;
    const int size = writeReceiverReport(buffer, 0x01020304, { &report, 1 });
    REQUIRE(size == 32);
    REQUIRE(toHex(buffer, size) == "81c90007010203040a0b0c0d4001020300011234000000550000000000000000");

    REQUIRE(writeReceiverReport(std::span<uint8_t>(buffer, 31), 1, { &report, 1 }) == -1);
}

TEST_CASE("NACK bytes", "[rtcp]")
{
    uint8_t buffer[64];

    // 100 and the two after it share an entry; 200 is too far away and gets its own.
    const std::vector<uint16_t> lost = { 100, 101, 116, 200 };
    const int size = writeNack(buffer, 0x01020304, 0x0A0B0C0D, lost);
    REQUIRE(size == 20);
    REQUIRE(toHex(buffer, size) == "81cd0004010203040a0b0c0d0064800100c80000");

    // The mask reaches across the wrap.
    const std::vector<uint16_t> wrapping = { 65535, 0, 2 };
    REQUIRE(writeNack(buffer, 1, 2, wrapping) == 16);
    REQUIRE(toHex(buffer + 12, 4) == "ffff0005");

    REQUIRE(writeNack(buffer, 1, 2, {}) == -1);
}

TEST_CASE("PLI bytes", "[rtcp]")
{
    uint8_t buffer[16];
    REQUIRE(writePli(buffer, 0x01020304, 0x0A0B0C0D) == 12);
    REQUIRE(toHex(buffer, 12) == "81ce0002010203040a0b0c0d");
    REQUIRE(writePli(std::span<uint8_t>(buffer, 11), 1, 2) == -1);
}

TEST_CASE("Transport feedback for packets in a row", "[rtcp][feedback]")
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    REQUIRE(!feedback.hasPending());
    REQUIRE(feedback.write(buffer, 1, 2) == 0);

    // Three packets a millisecond apart, the first 2 ms after a 64 ms boundary: one run of
    // "received", and deltas of 8, 4 and 4 quarter-milliseconds.
    feedback.onPacket(100, 322000);
    feedback.onPacket(101, 323000);
    feedback.onPacket(102, 324000);
    REQUIRE(feedback.hasPending());

    const int size = feedback.write(buffer, 0x01020304, 0x0A0B0C0D);
    REQUIRE(size == 28);
    REQUIRE(toHex(buffer, size) == "afcd0006010203040a0b0c0d00640003000005002003080404000003");
    REQUIRE(!feedback.hasPending());

    // Twenty in a row are still a single run.
    for (uint16_t i = 0; i < 20; i++)
        feedback.onPacket(static_cast<uint16_t>(200 + i), 640000 + i * 250);
    REQUIRE(feedback.write(buffer, 1, 2) == 44);
    REQUIRE(toHex(buffer + 12, 10) == "00c8001400000a012014");
}

TEST_CASE("Transport feedback marks what did not arrive", "[rtcp][feedback]")
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    feedback.onPacket(10, 640000);
    feedback.onPacket(12, 641000);

    // Received, missing, received: spelled out two bits each, and only two deltas.
    const int size = feedback.write(buffer, 0x01020304, 0x0A0B0C0D);
    REQUIRE(size == 24);
    REQUIRE(toHex(buffer, size) == "8fcd0005010203040a0b0c0d000a000300000a00d1000004");
}

TEST_CASE("Transport feedback carries long and backward gaps", "[rtcp][feedback]")
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    // The second packet is 100 ms behind the first, the third arrived before the second.
    feedback.onPacket(1, 0);
    feedback.onPacket(2, 100000);
    feedback.onPacket(3, 99000);

    const int size = feedback.write(buffer, 1, 2);
    REQUIRE(size == 28);
    // Small, large, large; then 0, +400 and -4 quarter-milliseconds.
    REQUIRE(toHex(buffer + 12, 16) == "0001000300000000da00000190fffc01");
}

TEST_CASE("Transport feedback reports each packet once", "[rtcp][feedback]")
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    feedback.onPacket(5, 64000);
    REQUIRE(feedback.write(buffer, 1, 2) == 24);
    REQUIRE(buffer[19] == uint8_t(0));

    // The next report starts at the next packet and counts up.
    feedback.onPacket(6, 128000);
    feedback.onPacket(7, 128250);
    REQUIRE(feedback.write(buffer, 1, 2) == 24);
    REQUIRE(toHex(buffer + 12, 12) == "000600020000020120020001");
    REQUIRE(feedback.write(buffer, 1, 2) == 0);

    // With no room nothing is written and nothing is forgotten.
    feedback.onPacket(8, 192000);
    REQUIRE(feedback.write(std::span<uint8_t>(buffer, 20), 1, 2) == -1);
    REQUIRE(feedback.hasPending());
    REQUIRE(feedback.write(buffer, 1, 2) == 24);
}

TEST_CASE("Transport feedback across the sequence wrap", "[rtcp][feedback]")
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    feedback.onPacket(65535, 64000);
    feedback.onPacket(0, 64250);
    feedback.onPacket(1, 64500);

    REQUIRE(feedback.write(buffer, 1, 2) == 28);
    REQUIRE(toHex(buffer + 12, 4) == "ffff0003");
}

// Tests from tst_VideoProtocol covering VideoPacketizer directly
namespace {
using ByteList = std::vector<uint8_t>;

void appendNalHelper(ByteList &frame, std::initializer_list<uint8_t> header, size_t body, bool longStart = false)
{
    frame.insert(frame.end(), longStart ? std::initializer_list<uint8_t>{0, 0, 0, 1} : std::initializer_list<uint8_t>{0, 0, 1});
    frame.insert(frame.end(), header);
    for (size_t i = 0; i < body; ++i)
        frame.push_back(static_cast<uint8_t>(1 + (i % 251)));
}

ByteList h264Sample()
{
    ByteList frame;
    appendNalHelper(frame, {0x67}, 9, true);
    appendNalHelper(frame, {0x68}, 5, true);
    appendNalHelper(frame, {0x65}, 4000, true);
    frame[frame.size() - 4000] |= 0x80;
    return frame;
}

ByteList h265Sample()
{
    ByteList frame;
    appendNalHelper(frame, {0x40, 0x01}, 9, true);
    appendNalHelper(frame, {0x42, 0x01}, 5, true);
    appendNalHelper(frame, {0x26, 0x01}, 4000, true);
    frame[frame.size() - 4000] |= 0x80;
    return frame;
}

ByteList av1Sample()
{
    ByteList frame = {0x0a};
    appendLeb128(frame, 3);
    frame.insert(frame.end(), {1, 2, 3});
    frame.push_back(0x32);
    appendLeb128(frame, 2500);
    for (int i = 0; i < 2500; ++i)
        frame.push_back(static_cast<uint8_t>(i));
    return frame;
}

ByteList rebuildPayloads(VideoCodec codec, const std::vector<ByteList> &payloads)
{
    std::vector<std::span<const uint8_t>> views;
    for (const auto &payload : payloads)
        views.emplace_back(payload);
    ByteList frame;
    REQUIRE(assembleFrame(codec, views, frame));
    return frame;
}
} // namespace

TEST_CASE("Video packetizers preserve RTP codec payload layouts", "[video][packetizer]")
{
    for (const auto &[codec, frame] : std::array<std::pair<VideoCodec, ByteList>, 4>{{{VideoCodec::H264, h264Sample()}, {VideoCodec::H265, h265Sample()}, {VideoCodec::VP8, ByteList(4000, 0x42)}, {VideoCodec::AV1, av1Sample()}}}) {
        VideoPacketizer packetizer(codec, 0x7ffe);
        auto payloads = packetizer.packetize(frame, true, 300);
        REQUIRE(payloads.size() > 1);
        for (const ByteList &payload : payloads)
            REQUIRE(payload.size() <= 300);
        ByteList assembled = rebuildPayloads(codec, payloads);
        if (codec == VideoCodec::AV1) {
            prepareForDecoder(codec, assembled);
            REQUIRE(assembled == frame);
        } else {
            REQUIRE(assembled == frame);
        }
        REQUIRE(startsKeyframe(codec, payloads.front()));
    }
}

TEST_CASE("Video packetizers reject malformed frames and honor boundaries", "[video][packetizer]")
{
    VideoPacketizer h264Packetizer(VideoCodec::H264);
    REQUIRE(h264Packetizer.packetize({std::array<uint8_t, 2>{0, 1}}, false, 300).empty());
    REQUIRE(h264Packetizer.packetize(h264Sample(), false, VideoPacketizer::MIN_PAYLOAD_SIZE - 1).empty());
    const std::array<uint8_t, 1> truncatedFu = {28};
    const std::array<uint8_t, 1> truncatedVp8 = {0x90};
    const std::array<uint8_t, 1> truncatedAv1 = {0x80};
    const std::array<std::span<const uint8_t>, 1> badH264 = {truncatedFu};
    const std::array<std::span<const uint8_t>, 1> badVp8 = {truncatedVp8};
    const std::array<std::span<const uint8_t>, 1> badAv1 = {truncatedAv1};
    ByteList output;
    REQUIRE_FALSE(assembleFrame(VideoCodec::H264, badH264, output));
    REQUIRE_FALSE(assembleFrame(VideoCodec::VP8, badVp8, output));
    REQUIRE_FALSE(assembleFrame(VideoCodec::AV1, badAv1, output));
}
