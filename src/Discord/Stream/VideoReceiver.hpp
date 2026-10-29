#pragma once

#include "Platform/Trackable.hpp"
#include "Platform/Timer.hpp"
#include <chrono>
#include <string>

#include <cstdint>
#include <map>
#include <optional>

namespace Styx::Platform { class WorkerLoop; }

namespace Styx {
namespace Discord {
namespace Stream {

// Receive side of one Go Live H264 stream: what libwebrtc's PacketBuffer + NackModule +
// H264 depacketizer do for a browser, cut down to one stream.
//
// Takes raw RTP (after SRTP), unwraps RTX, reorders by sequence number, NACKs holes, and
// emits whole access units in Annex B form with 4-byte start codes. That is byte-for-byte
// what WebRTC's depacketizer hands DAVE, so the frame decrypts. On unrecoverable loss it
// asks for a keyframe and resumes at the next one.
class VideoReceiver : public Platform::Trackable
{
public:
    explicit VideoReceiver(Platform::WorkerLoop *ownerLoop = nullptr);
    ~VideoReceiver() override;

    void setSsrcs(uint32_t videoSsrc, uint32_t rtxSsrc, uint32_t audioSsrc);
    void setLocalSsrc(uint32_t ssrc) { localSsrc = ssrc; }

    void handleRtp(const std::string &packet);
    void requestKeyframe();
    void reset();

    struct Stats
    {
        uint64_t packets = 0;
        uint64_t recovered = 0;
        uint64_t nacksSent = 0;
        uint64_t frames = 0;
        uint64_t framesDropped = 0;
        uint64_t keyframeRequests = 0;
    };
    [[nodiscard]] const Stats &stats() const { return counters; }

public:
    // An H264 access unit, still DAVE-encrypted when the session is E2EE.
    Platform::Signal<const std::string &, uint32_t, bool> frameReady;
    // An Opus packet from the stream's audio SSRC, still DAVE-encrypted when E2EE.
    Platform::Signal<uint32_t, uint16_t, uint32_t, const std::string &> audioReady;
    // RTCP feedback for the media server (NACK or PLI).
    Platform::Signal<const std::string &> sendRtcp;

private:
    struct Packet
    {
        uint32_t timestamp = 0;
        bool marker = false;
        bool keyframeStart = false; // carries SPS: an encoder starts every IDR access unit with it
        std::string payload;         // H264 RTP payload (no RTP header/extension/padding)
    };

    struct Missing
    {
        int64_t firstSeenMs = 0;
        int64_t lastNackMs = 0;
        int nackCount = 0;
    };

    void insertVideo(uint16_t sequence, uint32_t timestamp, bool marker, std::string payload);
    void assembleFrames();
    bool emitFrame(int64_t first, int64_t last);
    void resyncToKeyframe();
    void onTick();
    void sendNacks(int64_t nowMs);
    void sendPli();
    int64_t unwrap(uint16_t sequence);
    static bool payloadStartsKeyframe(const std::string &payload);

    uint32_t videoSsrc = 0;
    uint32_t rtxSsrc = 0;
    uint32_t audioSsrc = 0;
    uint32_t localSsrc = 1;

    std::map<int64_t, Packet> buffer;   // extended sequence -> packet
    std::map<int64_t, Missing> missing; // holes below the newest packet
    std::optional<int64_t> lastUnwrapped;
    std::optional<int64_t> nextFrameStart; // first sequence of the next frame to emit
    bool waitingForKeyframe = true;
    int64_t lastPliMs = -100000;

    std::chrono::steady_clock::time_point clock = std::chrono::steady_clock::now();
    Platform::WorkerLoop *ownerLoop;
    uint64_t workerTimer = 0;
    Platform::Timer tick;
    Stats counters;

    static constexpr int MAX_BUFFERED_PACKETS = 2048;
    static constexpr int NACK_INTERVAL_MS = 40;
    static constexpr int MAX_NACKS_PER_PACKET = 8;
    static constexpr int GIVE_UP_AFTER_MS = 400;
    static constexpr int PLI_INTERVAL_MS = 500;
};

} // namespace Stream
} // namespace Discord
} // namespace Styx
