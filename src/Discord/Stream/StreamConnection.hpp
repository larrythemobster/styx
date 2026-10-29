#pragma once

#include "Platform/Trackable.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Voice/VoiceEntities.hpp"
#include "Discord/Voice/VoiceEnums.hpp"

namespace Styx::Platform { class WorkerLoop; }

namespace Styx {
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
    std::string streamKey;
    std::string endpoint;
    std::string token;
    Core::Snowflake rtcServerId;
    Core::Snowflake selfUserId;
    Core::Snowflake streamerUserId;
    std::string voiceSessionId;
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
class StreamConnection : public Platform::Trackable
{
public:
    enum class Role {
        Sender,
        Viewer,
    };
    StreamConnection(Role role, const StreamSessionInfo &info, const Core::ProxyConfig &proxy,
                     Platform::WorkerLoop *ownerLoop = nullptr);
    ~StreamConnection() override;

    void start();
    void stop();

    [[nodiscard]] Role role() const { return connectionRole; }
    [[nodiscard]] const std::string &streamKey() const { return info.streamKey; }
    [[nodiscard]] bool isReady() const { return ready; }
    [[nodiscard]] bool isDaveEnabled() const;

    // Sender
    void setVideoAttributes(bool active, const VideoAttributes &attributes);
    void setSoundshare(bool active);
    void sendVideoFrame(const std::string &annexB, std::chrono::microseconds pts);
    void sendAudioFrame(const std::string &opus, std::chrono::microseconds pts);

    // Viewer
    void requestKeyframe();

    Platform::Signal<bool> readyChanged;
    Platform::Signal<const std::string &> closed;

    // Sender
    Platform::Signal<> keyframeRequested;
    Platform::Signal<unsigned int> bandwidthEstimate;

    // Viewer
    Platform::Signal<const std::string &, uint32_t, bool> videoFrame;
    Platform::Signal<uint32_t, uint16_t, uint32_t, const std::string &> audioPacket;

private:
    void onReady(const Voice::VoiceReady &data);
    void onSessionDescription(const Voice::SessionDescription &desc);
    void onVideo(const Voice::VideoData &data);
    void onTransportConnected();
    void onGatewayDisconnected(Voice::VoiceCloseCode code, const std::string &reason);
    void ensureDaveSession(uint16_t protocolVersion);
    void sendVideoState();
    void mapSsrc(uint32_t ssrc, Core::Snowflake userId);

    std::string daveEncrypt(bool video, uint32_t ssrc, const std::string &frame);
    bool daveDecrypt(bool video, uint32_t ssrc, std::string &frame);

    Role connectionRole;
    StreamSessionInfo info;
    Core::ProxyConfig proxy;
    Platform::WorkerLoop *ownerLoop;

    std::unique_ptr<Voice::VoiceGateway> gateway;
    std::unique_ptr<StreamTransport> transport;
    std::unique_ptr<VideoReceiver> receiver;
    std::unique_ptr<Voice::DaveSession> daveSession;

    uint32_t audioSsrc = 0;
    uint32_t videoSsrc = 0;
    uint32_t rtxSsrc = 0;
    bool ready = false;
    bool stopped = false;

    bool videoActive = false;
    VideoAttributes attributes;

    // the streamer's SSRCs, for a viewer
    uint32_t remoteAudioSsrc = 0;
    uint32_t remoteVideoSsrc = 0;
    uint32_t remoteRtxSsrc = 0;

    std::set<std::string> connectedUserIds;
    std::unordered_map<uint32_t, uint64_t> ssrcToUserId;
};

} // namespace Stream
} // namespace Discord
} // namespace Styx
