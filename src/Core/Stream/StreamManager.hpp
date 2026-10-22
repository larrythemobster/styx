#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Events.hpp"
#include "ScreenCapture.hpp"
#include "StreamQuality.hpp"

#include "Platform/WorkerLoop.hpp"

namespace Styx {
namespace Discord {
class Client;
namespace Stream {
class StreamConnection;
}
} // namespace Discord

namespace Core {
namespace Audio {
class VoiceManager;
}
namespace Stream {

class StreamSender;
class StreamDecoder;

// Go Live for one account: the stream we send and the streams we watch. Talks to the main
// gateway (ops 18-22 and the STREAM_* dispatches) and runs every media session on a
// dedicated thread; capture/encode and each decoder get threads of their own.
class StreamManager : public QObject
{
    Q_OBJECT
public:
    StreamManager(Snowflake accountId, const ProxyConfig &proxy, Discord::Client *client,
                  Audio::VoiceManager *voice, QObject *parent = nullptr);
    ~StreamManager() override;

    // Whether this machine and session can stream right now; reason says why not.
    bool canStream(QString *reason = nullptr) const;
    bool canWatch(QString *reason = nullptr) const;

    void startStreaming(const CaptureSource &source, const StreamQuality &quality, bool includeAudio);
    void stopStreaming();
    [[nodiscard]] bool isStreaming() const { return outgoing.active; }
    [[nodiscard]] const QString &streamingDescription() const { return outgoing.description; }

    void watchStream(Snowflake userId);
    void stopWatching(Snowflake userId);
    [[nodiscard]] bool isWatching(Snowflake userId) const { return watched.contains(userId); }
    void setViewerSize(Snowflake userId, const QSize &size);
    void requestViewerKeyframe(Snowflake userId);

    void handleStreamCreate(const Discord::StreamCreate &event);
    void handleStreamServerUpdate(const Discord::StreamServerUpdate &event);
    void handleStreamDelete(const Discord::StreamDelete &event);

    // The voice call ended: every stream goes with it.
    void stopAll();

signals:
    void streamingChanged(bool streaming);
    void streamingStatus(const QString &status);
    void streamingFailed(const QString &reason);

    void watchStarted(Snowflake userId);
    void watchConnected(Snowflake userId);
    void watchEnded(Snowflake userId, const QString &reason);
    void viewerFrame(Snowflake userId, const QImage &image, const QSize &sourceSize);

private:
    struct Session
    {
        QString key;
        Snowflake userId;
        Snowflake rtcServerId;
        QString endpoint;
        QString token;
        bool active = false;
        QPointer<Discord::Stream::StreamConnection> connection;
        QPointer<StreamDecoder> decoder;
        std::shared_ptr<Platform::WorkerLoop> decoderLoop;
        // outgoing only
        QString description;
        bool audio = false;
        int width = 0;
        int height = 0;
        int framerate = 0;
        int maxBitrate = 0;
    };

    QString streamKeyFor(Snowflake userId) const;
    void maybeConnect(Session &session, bool sender);
    void connectSender(Session &session);
    void connectViewer(Session &session);
    void destroyConnection(Session &session);
    void endWatch(Snowflake userId, const QString &reason, bool notifyServer);
    void ensureStreamLoop();
    void ensureSenderLoop();

    Snowflake accountId;
    ProxyConfig proxy;
    QPointer<Discord::Client> client;
    QPointer<Audio::VoiceManager> voice;

    std::unique_ptr<Platform::WorkerLoop> streamLoop;
    std::unique_ptr<Platform::WorkerLoop> senderLoop;
    StreamSender *sender = nullptr;

    Session outgoing;
    bool senderStarted = false;
    QHash<Snowflake, Session> watched;
};

} // namespace Stream
} // namespace Core
} // namespace Styx
