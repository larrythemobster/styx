#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Events.hpp"
#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include "Platform/Trackable.hpp"
#include "Platform/WorkerLoop.hpp"
#include "ScreenCapture.hpp"
#include "StreamQuality.hpp"

namespace Styx {
namespace Discord {
class Client;
namespace Stream { class StreamConnection; }
}
namespace Core {
namespace Audio { class VoiceManager; }
namespace Stream {

class StreamSender;
class StreamDecoder;

class StreamManager : public Platform::Trackable
{
public:
    StreamManager(Snowflake accountId, const ProxyConfig &proxy, Discord::Client *client,
                  Audio::VoiceManager *voice);
    ~StreamManager() override;

    bool canStream(std::string *reason = nullptr) const;
    bool canWatch(std::string *reason = nullptr) const;
    void startStreaming(const CaptureSource &source, const StreamQuality &quality, bool includeAudio);
    void stopStreaming();
    [[nodiscard]] bool isStreaming() const { return outgoing.active; }
    [[nodiscard]] const std::string &streamingDescription() const { return outgoing.description; }

    void watchStream(Snowflake userId);
    void stopWatching(Snowflake userId);
    [[nodiscard]] bool isWatching(Snowflake userId) const { return watched.contains(userId); }
    void setViewerSize(Snowflake userId, const Platform::Size &size);
    void requestViewerKeyframe(Snowflake userId);
    void handleStreamCreate(const Discord::StreamCreate &event);
    void handleStreamServerUpdate(const Discord::StreamServerUpdate &event);
    void handleStreamDelete(const Discord::StreamDelete &event);
    void stopAll();

    Platform::Signal<bool> streamingChanged;
    Platform::Signal<const std::string &> streamingStatus;
    Platform::Signal<const std::string &> streamingFailed;
    Platform::Signal<Snowflake> watchStarted;
    Platform::Signal<Snowflake> watchConnected;
    Platform::Signal<Snowflake, const std::string &> watchEnded;
    Platform::Signal<Snowflake, const Platform::ImageBuffer &, const Platform::Size &> viewerFrame;

private:
    struct Session {
        std::string key;
        Snowflake userId;
        Snowflake rtcServerId;
        std::string endpoint;
        std::string token;
        bool active = false;
        std::shared_ptr<Discord::Stream::StreamConnection> connection;
        std::shared_ptr<StreamDecoder> decoder;
        std::shared_ptr<Platform::WorkerLoop> decoderLoop;
        std::string description;
        bool audio = false;
        int width = 0;
        int height = 0;
        int framerate = 0;
        int maxBitrate = 0;
    };

    std::string streamKeyFor(Snowflake userId) const;
    void maybeConnect(Session &session, bool sender);
    void connectSender(Session &session);
    void connectViewer(Session &session);
    void destroyConnection(Session &session);
    void endWatch(Snowflake userId, const std::string &reason, bool notifyServer);
    void ensureStreamLoop();
    void ensureSenderLoop();

    Snowflake accountId;
    ProxyConfig proxy;
    Platform::Guard<Discord::Client> client;
    Platform::Guard<Audio::VoiceManager> voice;
    std::unique_ptr<Platform::WorkerLoop> streamLoop;
    std::unique_ptr<Platform::WorkerLoop> senderLoop;
    std::shared_ptr<StreamSender> sender;
    Session outgoing;
    bool senderStarted = false;
    std::unordered_map<Snowflake, Session> watched;
};

} // namespace Stream
} // namespace Core
} // namespace Styx
