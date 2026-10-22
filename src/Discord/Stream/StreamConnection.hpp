#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <chrono>
#include <memory>
#include <set>
#include <string>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Voice/VoiceEntities.hpp"
#include "Discord/Voice/VoiceEnums.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {
class VoiceGateway;
class DaveSession;
} // namespace Voice

namespace Stream {

class StreamTransport;
class VideoReceiver;

struct StreamSessionInfo
{
    QString streamKey;
    QString endpoint;
    QString token;
    Core::Snowflake rtcServerId;
    Core::Snowflake selfUserId;
    Core::Snowflake streamerUserId;
    QString voiceSessionId;
    int maxBitrate = 0; // sender: peak video bitrate
};

struct VideoAttributes
{
    int width = 0;
    int height = 0;
    int framerate = 0;
    int maxBitrate = 0;
};

// One Go Live media session: its own voice gateway (identified with video), a WebRTC
// transport and its own DAVE group. A sender pushes encoded frames in; a viewer gets
// decrypted H264 access units and Opus packets out.
class StreamConnection : public QObject
{
    Q_OBJECT
public:
    enum class Role {
        Sender,
        Viewer,
    };
    Q_ENUM(Role)

    StreamConnection(Role role, const StreamSessionInfo &info, const Core::ProxyConfig &proxy, QObject *parent = nullptr);
    ~StreamConnection() override;

    void start();
    void stop();

    [[nodiscard]] Role role() const { return connectionRole; }
    [[nodiscard]] const QString &streamKey() const { return info.streamKey; }
    [[nodiscard]] bool isReady() const { return ready; }
    [[nodiscard]] bool isDaveEnabled() const;

    // Sender
    void setVideoAttributes(bool active, const VideoAttributes &attributes);
    void setSoundshare(bool active);
    void sendVideoFrame(const QByteArray &annexB, std::chrono::microseconds pts);
    void sendAudioFrame(const QByteArray &opus, std::chrono::microseconds pts);

    // Viewer
    void requestKeyframe();

signals:
    void readyChanged(bool ready);
    void closed(const QString &reason);

    // Sender
    void keyframeRequested();
    void bandwidthEstimate(unsigned int bitsPerSecond);

    // Viewer
    void videoFrame(const QByteArray &annexB, quint32 rtpTimestamp, bool keyframe);
    void audioPacket(quint32 ssrc, quint16 sequence, quint32 rtpTimestamp, const QByteArray &opus);

private:
    void onReady(const Voice::VoiceReady &data);
    void onSessionDescription(const Voice::SessionDescription &desc);
    void onVideo(const Voice::VideoData &data);
    void onTransportConnected();
    void onGatewayDisconnected(Voice::VoiceCloseCode code, const QString &reason);
    void ensureDaveSession(uint16_t protocolVersion);
    void sendVideoState();
    void mapSsrc(quint32 ssrc, Core::Snowflake userId);

    QByteArray daveEncrypt(bool video, quint32 ssrc, const QByteArray &frame);
    bool daveDecrypt(bool video, quint32 ssrc, QByteArray &frame);

    Role connectionRole;
    StreamSessionInfo info;
    Core::ProxyConfig proxy;

    Voice::VoiceGateway *gateway = nullptr;
    StreamTransport *transport = nullptr;
    VideoReceiver *receiver = nullptr;
    std::unique_ptr<Voice::DaveSession> daveSession;

    quint32 audioSsrc = 0;
    quint32 videoSsrc = 0;
    quint32 rtxSsrc = 0;
    bool ready = false;
    bool stopped = false;

    bool videoActive = false;
    VideoAttributes attributes;

    // the streamer's SSRCs, for a viewer
    quint32 remoteAudioSsrc = 0;
    quint32 remoteVideoSsrc = 0;
    quint32 remoteRtxSsrc = 0;

    std::set<std::string> connectedUserIds;
    QHash<quint32, uint64_t> ssrcToUserId;
};

} // namespace Stream
} // namespace Discord
} // namespace Acheron
