#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <cstdint>
#include <map>
#include <optional>

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
class VideoReceiver : public QObject
{
    Q_OBJECT
public:
    explicit VideoReceiver(QObject *parent = nullptr);

    void setSsrcs(quint32 videoSsrc, quint32 rtxSsrc, quint32 audioSsrc);
    void setLocalSsrc(quint32 ssrc) { localSsrc = ssrc; }

    void handleRtp(const QByteArray &packet);
    void requestKeyframe();
    void reset();

    struct Stats
    {
        quint64 packets = 0;
        quint64 recovered = 0;
        quint64 nacksSent = 0;
        quint64 frames = 0;
        quint64 framesDropped = 0;
        quint64 keyframeRequests = 0;
    };
    [[nodiscard]] const Stats &stats() const { return counters; }

signals:
    // An H264 access unit, still DAVE-encrypted when the session is E2EE.
    void frameReady(const QByteArray &annexB, quint32 rtpTimestamp, bool keyframe);
    // An Opus packet from the stream's audio SSRC, still DAVE-encrypted when E2EE.
    void audioReady(quint32 ssrc, quint16 sequence, quint32 rtpTimestamp, const QByteArray &opus);
    // RTCP feedback for the media server (NACK or PLI).
    void sendRtcp(const QByteArray &packet);

private:
    struct Packet
    {
        quint32 timestamp = 0;
        bool marker = false;
        bool keyframeStart = false; // carries SPS: an encoder starts every IDR access unit with it
        QByteArray payload;         // H264 RTP payload (no RTP header/extension/padding)
    };

    struct Missing
    {
        qint64 firstSeenMs = 0;
        qint64 lastNackMs = 0;
        int nackCount = 0;
    };

    void insertVideo(quint16 sequence, quint32 timestamp, bool marker, QByteArray payload);
    void assembleFrames();
    bool emitFrame(int64_t first, int64_t last);
    void resyncToKeyframe();
    void onTick();
    void sendNacks(qint64 nowMs);
    void sendPli();
    int64_t unwrap(quint16 sequence);
    static bool payloadStartsKeyframe(const QByteArray &payload);

    quint32 videoSsrc = 0;
    quint32 rtxSsrc = 0;
    quint32 audioSsrc = 0;
    quint32 localSsrc = 1;

    std::map<int64_t, Packet> buffer;   // extended sequence -> packet
    std::map<int64_t, Missing> missing; // holes below the newest packet
    std::optional<int64_t> lastUnwrapped;
    std::optional<int64_t> nextFrameStart; // first sequence of the next frame to emit
    bool waitingForKeyframe = true;
    qint64 lastPliMs = -100000;

    QElapsedTimer clock;
    QTimer tick{ this };
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
