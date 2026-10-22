#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

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

namespace Acheron {
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
// Lives on the stream thread. libdatachannel calls back on its own threads; everything that
// reaches Qt is queued onto this object's thread. send*() are safe from any thread.
class StreamTransport : public QObject
{
    Q_OBJECT
public:
    enum class Role {
        Sender,
        Viewer,
    };

    explicit StreamTransport(Role role, QObject *parent = nullptr);
    ~StreamTransport() override;

    // Codecs offered in SELECT_PROTOCOL. Only H264 is offered so every stream we watch is
    // H264 and every viewer of ours decodes it.
    static QList<Voice::Codec> codecs();

    // Before createOffer(): the SSRCs READY assigned. A viewer passes its (unused) audio SSRC.
    void setLocalSsrcs(quint32 audioSsrc, quint32 videoSsrc, quint32 rtxSsrc);
    // Sender, before createOffer(): the stream's peak bitrate, for pacing.
    void setMaxBitrate(int bitsPerSecond) { maxBitrate = bitsPerSecond; }

    void createOffer();
    bool applyServerSdp(const QString &serverSdp);
    void close();

    [[nodiscard]] bool isConnected() const { return connectedFlag.load(); }

    // Sender. frame is one encoded (and DAVE-encrypted) access unit in Annex B form.
    void sendVideoFrame(const QByteArray &frame, std::chrono::microseconds pts);
    void sendAudioFrame(const QByteArray &opus, std::chrono::microseconds pts);

    // Viewer. Sends one RTCP feedback packet (NACK, PLI, RR).
    void sendRtcp(const QByteArray &packet);

signals:
    void localOfferReady(const QString &sdp);
    void connected();
    void failed(const QString &reason);

    // Sender: the media server asked for a keyframe (PLI/FIR) or estimated our bandwidth (REMB).
    void keyframeRequested();
    void bandwidthEstimate(unsigned int bitsPerSecond);

    // Viewer: one decrypted RTP packet, header included. Emitted from a libdatachannel
    // thread, so receivers get it queued.
    void rtpReceived(const QByteArray &packet);

private:
    void setupSenderChain();
    void postToThread(std::function<void()> fn);

    Role role;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> audioTrack;
    std::shared_ptr<rtc::Track> videoTrack;

    quint32 audioSsrc = 0;
    quint32 videoSsrc = 0;
    quint32 rtxSsrc = 0;
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
} // namespace Acheron
