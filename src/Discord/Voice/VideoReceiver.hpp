// Ported from ouwou upstream/video-streams (71b4907); original implementation retained.
#pragma once


#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "PacketBuffer.hpp"
#include "Rtcp.hpp"
#include "VideoDepacketizer.hpp"

namespace Styx::Discord::Voice {

struct EncodedVideoFrame
{
    std::vector<uint8_t> data;
    uint32_t rtpTimestamp = 0;
    bool keyframe = false;
};

// Turns the RTP packets of one video SSRC into complete frames, in decode order. It holds
// packets that arrive early, names the ones to ask the sender for again, and when a loss cannot
// be repaired it discards everything up to the next key frame and asks for one.
//
// It keeps no clock and no timer of its own: the owner passes the time in and calls poll().
class VideoReceiver
{
public:
    struct Feedback
    {
        std::vector<uint16_t> nacks;
        bool keyframeRequest = false;
    };

    struct Stats
    {
        uint64_t packets = 0;
        uint64_t bytes = 0;
        uint64_t frames = 0;
        uint64_t keyframes = 0;
        uint64_t retransmitted = 0;
        uint64_t duplicates = 0;
        uint64_t strays = 0;
        uint64_t nacked = 0;
        uint64_t keyframeRequests = 0;
        uint64_t resyncs = 0;
    };

    VideoReceiver(uint32_t ssrc, VideoCodec codec);

    [[nodiscard]] uint32_t ssrc() const { return mediaSsrc; }
    [[nodiscard]] VideoCodec codec() const { return videoCodec; }

    // `payload` holds exactly the RTP payload: header, extensions and padding already removed,
    // and for a retransmission the original sequence number restored.
    void insert(uint16_t sequence, uint32_t timestamp, bool marker, PacketPool::Handle payload, int64_t nowMs, bool retransmission = false);

    [[nodiscard]] std::vector<EncodedVideoFrame> takeFrames();
    [[nodiscard]] Feedback poll(int64_t nowMs);

    // What came out could not be used (it failed to decrypt or decode): start over from a key frame.
    void requestKeyframe();

    [[nodiscard]] const Stats &stats() const { return counters; }
    [[nodiscard]] ReceptionReport receptionReport();

    static constexpr int REORDER_GRACE_MS = 10;
    static constexpr int NACK_RETRY_MS = 100;
    static constexpr int NACK_MAX_REQUESTS = 10;
    static constexpr int NACK_GIVE_UP_MS = 1000;
    static constexpr int MAX_NACKS_PER_POLL = 250;
    // As the official client's WebRTC has them: a key frame is asked for again every 200 ms
    // while one is needed, but not of a stream nothing has come from for five seconds.
    static constexpr int KEYFRAME_REQUEST_INTERVAL_MS = 200;
    static constexpr int SILENT_AFTER_MS = 5000;
    static constexpr int MAX_SEQUENCE_JUMP = 3000;
    static constexpr int MAX_HELD_PACKETS = 4000;

private:
    struct Held
    {
        PacketPool::Handle payload;
        uint32_t timestamp = 0;
        bool marker = false;
    };

    struct Missing
    {
        int64_t noticedMs = 0;
        int64_t requestedMs = 0;
        int requests = 0;
    };

    [[nodiscard]] int64_t unwrap(uint16_t sequence) const;
    void assemble();
    void resync();
    void noteArrival(uint32_t timestamp, int64_t nowMs);

    uint32_t mediaSsrc;
    VideoCodec videoCodec;

    std::map<int64_t, Held> held;
    std::map<int64_t, Missing> missing;
    std::vector<EncodedVideoFrame> ready;

    // The next sequence number a frame may start at; -1 while waiting for a key frame.
    int64_t nextSequence = -1;
    int64_t highestSequence = -1;
    // The number a packet from far outside the expected range would be followed by, were it
    // the first of a sender's new numbering and not a stray.
    std::optional<uint16_t> sequenceAfterFarPacket;
    bool keyframeWanted = true;
    int64_t keyframeRequestedMs = -1;
    int64_t lastPacketMs = -1;

    Stats counters;

    // Receiver report bookkeeping (RFC 3550, A.3 and A.8).
    int64_t baseSequence = -1;
    uint64_t received = 0;
    int64_t expectedAtLastReport = 0;
    uint64_t receivedAtLastReport = 0;
    double jitter = 0;
    int64_t lastTransit = 0;
    bool haveTransit = false;
};

} // namespace Styx::Discord::Voice
