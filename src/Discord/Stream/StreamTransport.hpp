#pragma once

#include "Platform/Trackable.hpp"
#include <string>
#include <vector>
#include <cstdint>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>

#include "Discord/Voice/VoiceEntities.hpp"

namespace rtc {
class PeerConnection;
class Track;
class RtpPacketizationConfig;
} // namespace rtc
namespace Styx::Platform { class WorkerLoop; }

namespace Styx {
namespace Discord {
namespace Stream {

// Payload types Discord's media servers use for every client.
inline constexpr int OPUS_PAYLOAD_TYPE = 120;
inline constexpr int H264_PAYLOAD_TYPE = 101;
inline constexpr int H264_RTX_PAYLOAD_TYPE = 102;
inline constexpr uint32_t VIDEO_CLOCK_RATE = 90000;
inline constexpr uint32_t OPUS_CLOCK_RATE = 48000;

// WebRTC transport for one Go Live media session: ICE + DTLS-SRTP through libdatachannel,
// one audio (mid 0) and one video (mid 1) m-line. The media server is ICE-lite and only ever
// hands back fragments of an SDP, so the answer is rebuilt from them (applyServerSdp).
//
// Lives on the stream thread. libdatachannel callbacks are queued to its owner loop.
// send*() are safe from any thread.
class StreamTransport : public Platform::Trackable
{
public:
    enum class Role {
        Sender,
        Viewer,
    };

    explicit StreamTransport(Role role, Platform::WorkerLoop *ownerLoop = nullptr);
    ~StreamTransport() override;

    // Codecs offered in SELECT_PROTOCOL. Only H264 is offered so every stream we watch is
    // H264 and every viewer of ours decodes it.
    static std::vector<Voice::Codec> codecs();

    // Before createOffer(): the SSRCs READY assigned. A viewer passes its (unused) audio SSRC.
    void setLocalSsrcs(uint32_t audioSsrc, uint32_t videoSsrc, uint32_t rtxSsrc);
    // Sender, before createOffer(): the stream's peak bitrate, for pacing.
    void setMaxBitrate(int bitsPerSecond) { maxBitrate = bitsPerSecond; }

    void createOffer();
    bool applyServerSdp(const std::string &serverSdp);
    void close();

    [[nodiscard]] bool isConnected() const { return connectedFlag.load(); }

    // Sender. frame is one encoded (and DAVE-encrypted) access unit in Annex B form.
    void sendVideoFrame(const std::string &frame, std::chrono::microseconds pts);
    void sendAudioFrame(const std::string &opus, std::chrono::microseconds pts);

    // Viewer. Sends one RTCP feedback packet (NACK, PLI, RR).
    void sendRtcp(const std::string &packet);

public:
    Platform::Signal<const std::string &> localOfferReady;
    Platform::Signal<> connected;
    Platform::Signal<const std::string &> failed;

    // Sender: the media server asked for a keyframe (PLI/FIR) or estimated our bandwidth (REMB).
    Platform::Signal<> keyframeRequested;
    Platform::Signal<unsigned int> bandwidthEstimate;

    // Viewer: one decrypted RTP packet, header included. Emitted from a libdatachannel
    // thread, so receivers get it queued.
    Platform::Signal<const std::string &> rtpReceived;

private:
    void setupSenderChain();
    void postToThread(std::function<void()> fn);

    Role role;
    Platform::WorkerLoop *ownerLoop;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> audioTrack;
    std::shared_ptr<rtc::Track> videoTrack;

    uint32_t audioSsrc = 0;
    uint32_t videoSsrc = 0;
    uint32_t rtxSsrc = 0;
    int maxBitrate = 0;

    std::atomic<bool> connectedFlag{ false };

    // Shared with libdatachannel callbacks. close() flips alive under the mutex, so once it
    // returns no callback is touching this object and none will start.
    struct Guard
    {
        std::mutex mutex;
        bool alive = true;
    };
    std::shared_ptr<Guard> guard;
};

} // namespace Stream
} // namespace Discord
} // namespace Styx
