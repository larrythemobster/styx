#include "StreamManager.hpp"

#include "StreamDecoder.hpp"
#include "StreamSender.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Logging.hpp"
#include "Discord/Client.hpp"
#include "Discord/Stream/StreamConnection.hpp"

#include <QThread>

#ifdef Q_OS_WIN
#include <objbase.h>
#endif

namespace Acheron {
namespace Core {
namespace Stream {

using Discord::Stream::StreamConnection;

StreamManager::StreamManager(Snowflake accountId, const ProxyConfig &proxy, Discord::Client *client,
                             Audio::VoiceManager *voice, QObject *parent)
    : QObject(parent), accountId(accountId), proxy(proxy), client(client), voice(voice)
{
}

StreamManager::~StreamManager()
{
    stopAll();
    if (sender) {
        QMetaObject::invokeMethod(sender, [s = sender] { s->stop(); }, Qt::BlockingQueuedConnection);
        sender->deleteLater();
        sender = nullptr;
    }
    for (QThread *thread : { senderThread, streamThread }) {
        if (!thread)
            continue;
        thread->quit();
        thread->wait(5000);
        delete thread;
    }
}

bool StreamManager::canWatch(QString *reason) const
{
    auto fail = [reason](const QString &text) {
        if (reason)
            *reason = text;
        return false;
    };
    if (!voice || !voice->isConnected() || voice->sessionId().isEmpty())
        return fail(tr("Join a voice channel first."));
    // the WebRTC transport has no way through a proxy; refusing beats leaking the real address
    if (proxy.enabled())
        return fail(tr("Go Live does not work through a proxy: the stream would bypass it."));
    return true;
}

bool StreamManager::canStream(QString *reason) const
{
    if (!canWatch(reason))
        return false;
    if (!isScreenCaptureSupported()) {
        if (reason)
            *reason = tr("Screen capture needs Windows 10 version 1903 or later.");
        return false;
    }
    return true;
}

QString StreamManager::streamKeyFor(Snowflake userId) const
{
    const Snowflake guild = voice ? voice->currentGuildId() : Snowflake::Invalid;
    const Snowflake channel = voice ? voice->currentChannelId() : Snowflake::Invalid;
    if (guild.isValid())
        return QStringLiteral("guild:%1:%2:%3").arg(QString::number(guild), QString::number(channel), QString::number(userId));
    return QStringLiteral("call:%1:%2").arg(QString::number(channel), QString::number(userId));
}

void StreamManager::ensureStreamThread()
{
    if (streamThread)
        return;
    streamThread = new QThread;
    streamThread->setObjectName(QStringLiteral("StreamThread"));
    streamThread->start();
}

void StreamManager::ensureSenderThread()
{
    if (senderThread)
        return;
    senderThread = new QThread;
    senderThread->setObjectName(QStringLiteral("StreamCaptureThread"));
#ifdef Q_OS_WIN
    // Windows Graphics Capture and WASAPI activation want a multithreaded apartment
    connect(senderThread, &QThread::started, senderThread, [] { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }, Qt::DirectConnection);
    connect(senderThread, &QThread::finished, senderThread, [] { CoUninitialize(); }, Qt::DirectConnection);
#endif
    senderThread->start();

    sender = new StreamSender;
    sender->moveToThread(senderThread);

    connect(sender, &StreamSender::started, this,
            [this](int width, int height, int framerate, int maxBitrate, bool audio, const QString &encoderName) {
                if (!outgoing.active)
                    return;
                senderStarted = true;
                outgoing.width = width;
                outgoing.height = height;
                outgoing.framerate = framerate;
                outgoing.maxBitrate = maxBitrate;
                outgoing.audio = audio;
                outgoing.description = tr("%1×%2 at %3 fps, %4%5")
                                               .arg(width)
                                               .arg(height)
                                               .arg(framerate)
                                               .arg(encoderName, audio ? tr(", with audio") : QString());
                qCInfo(LogStream) << "Capture running:" << outgoing.description << "- creating stream" << outgoing.key;

                if (client) {
                    client->sendStreamCreate(voice->currentGuildId(), voice->currentChannelId());
                    client->sendStreamSetPaused(outgoing.key, false);
                }
                emit streamingStatus(tr("Connecting…"));
            });
    connect(sender, &StreamSender::failed, this, [this](const QString &reason) {
        qCWarning(LogStream) << "Stream sender failed:" << reason;
        stopStreaming();
        emit streamingFailed(reason);
    });
    connect(sender, &StreamSender::sourceClosed, this, [this] {
        qCInfo(LogStream) << "Shared window closed, ending stream";
        stopStreaming();
        emit streamingFailed(tr("The shared window was closed."));
    });
}

void StreamManager::startStreaming(const CaptureSource &source, const StreamQuality &quality, bool includeAudio)
{
    QString reason;
    if (!canStream(&reason)) {
        emit streamingFailed(reason);
        return;
    }
    if (outgoing.active)
        stopStreaming();

    ensureStreamThread();
    ensureSenderThread();

    outgoing = Session{};
    outgoing.key = streamKeyFor(accountId);
    outgoing.userId = accountId;
    outgoing.active = true;
    senderStarted = false;

    emit streamingChanged(true);
    emit streamingStatus(tr("Starting capture…"));

    QMetaObject::invokeMethod(sender, [s = sender, source, quality, includeAudio] { s->start(source, quality, includeAudio); });
}

void StreamManager::stopStreaming()
{
    if (!outgoing.active)
        return;

    qCInfo(LogStream) << "Stopping stream" << outgoing.key;
    if (senderStarted && client)
        client->sendStreamDelete(outgoing.key);

    destroyConnection(outgoing);
    if (sender)
        QMetaObject::invokeMethod(sender, [s = sender] { s->stop(); }, Qt::BlockingQueuedConnection);

    outgoing = Session{};
    senderStarted = false;
    emit streamingChanged(false);
}

void StreamManager::watchStream(Snowflake userId)
{
    QString reason;
    if (!canWatch(&reason)) {
        emit watchEnded(userId, reason);
        return;
    }
    if (watched.contains(userId) || userId == accountId)
        return;

    ensureStreamThread();

    Session session;
    session.key = streamKeyFor(userId);
    session.userId = userId;
    session.active = true;
    watched.insert(userId, session);

    qCInfo(LogStream) << "Watching stream" << session.key;
    if (client)
        client->sendStreamWatch(session.key);
    emit watchStarted(userId);
}

void StreamManager::stopWatching(Snowflake userId)
{
    endWatch(userId, QString(), true);
}

void StreamManager::endWatch(Snowflake userId, const QString &reason, bool notifyServer)
{
    auto it = watched.find(userId);
    if (it == watched.end())
        return;
    Session session = it.value();
    watched.erase(it);

    qCInfo(LogStream) << "Leaving stream" << session.key << reason;
    if (notifyServer && client)
        client->sendStreamDelete(session.key);
    destroyConnection(session);
    emit watchEnded(userId, reason);
}

void StreamManager::setViewerSize(Snowflake userId, const QSize &size)
{
    auto it = watched.find(userId);
    if (it != watched.end() && it->decoder)
        it->decoder->setTargetSize(size);
}

void StreamManager::requestViewerKeyframe(Snowflake userId)
{
    auto it = watched.find(userId);
    if (it != watched.end() && it->connection)
        QMetaObject::invokeMethod(it->connection, [c = it->connection] { if (c) c->requestKeyframe(); });
}

void StreamManager::stopAll()
{
    stopStreaming();
    const auto users = watched.keys();
    for (Snowflake userId : users)
        endWatch(userId, tr("You left the voice channel."), false);
}

void StreamManager::handleStreamCreate(const Discord::StreamCreate &event)
{
    const QString key = event.streamKey.get();
    Session *session = nullptr;
    bool isSender = false;
    if (outgoing.active && outgoing.key == key) {
        session = &outgoing;
        isSender = true;
    } else {
        for (auto &candidate : watched) {
            if (candidate.key == key) {
                session = &candidate;
                break;
            }
        }
    }
    if (!session)
        return;

    qCInfo(LogStream) << "STREAM_CREATE" << key << "rtc server" << event.rtcServerId.get();
    if (session->connection && session->rtcServerId != event.rtcServerId.get())
        destroyConnection(*session);
    session->rtcServerId = event.rtcServerId.get();
    maybeConnect(*session, isSender);
}

void StreamManager::handleStreamServerUpdate(const Discord::StreamServerUpdate &event)
{
    const QString key = event.streamKey.get();
    Session *session = nullptr;
    bool isSender = false;
    if (outgoing.active && outgoing.key == key) {
        session = &outgoing;
        isSender = true;
    } else {
        for (auto &candidate : watched) {
            if (candidate.key == key) {
                session = &candidate;
                break;
            }
        }
    }
    if (!session)
        return;

    if (event.endpoint.isNull() || event.endpoint.get().isEmpty()) {
        qCInfo(LogStream) << "STREAM_SERVER_UPDATE without endpoint for" << key << "- waiting";
        return;
    }

    qCInfo(LogStream) << "STREAM_SERVER_UPDATE" << key << "endpoint" << event.endpoint.get();
    // a second update means the media server moved; reconnect there
    if (session->connection)
        destroyConnection(*session);
    session->endpoint = event.endpoint.get();
    session->token = event.token.get();
    maybeConnect(*session, isSender);
}

void StreamManager::handleStreamDelete(const Discord::StreamDelete &event)
{
    const QString key = event.streamKey.get();
    const QString reason = event.reason.hasValue() && !event.reason.isNull() ? event.reason.get() : QString();
    qCInfo(LogStream) << "STREAM_DELETE" << key << reason;

    if (outgoing.active && outgoing.key == key) {
        senderStarted = false; // the server already dropped it
        stopStreaming();
        if (!reason.isEmpty() && reason != QLatin1String("user_requested"))
            emit streamingFailed(tr("Discord ended the stream (%1).").arg(reason));
        return;
    }
    for (auto it = watched.begin(); it != watched.end(); ++it) {
        if (it->key == key) {
            const Snowflake userId = it.key();
            endWatch(userId, reason == QLatin1String("stream_ended") || reason.isEmpty() ? tr("The stream ended.")
                                                                                          : tr("Stream closed (%1).").arg(reason),
                     false);
            return;
        }
    }
}

void StreamManager::maybeConnect(Session &session, bool isSender)
{
    if (session.connection || !session.rtcServerId.isValid() || session.endpoint.isEmpty() || !voice
        || voice->sessionId().isEmpty())
        return;
    if (isSender)
        connectSender(session);
    else
        connectViewer(session);
}

void StreamManager::connectSender(Session &session)
{
    Discord::Stream::StreamSessionInfo info;
    info.streamKey = session.key;
    info.endpoint = session.endpoint;
    info.token = session.token;
    info.rtcServerId = session.rtcServerId;
    info.selfUserId = accountId;
    info.streamerUserId = accountId;
    info.voiceSessionId = voice->sessionId();
    info.maxBitrate = session.maxBitrate;

    auto *connection = new StreamConnection(StreamConnection::Role::Sender, info, proxy);
    connection->moveToThread(streamThread);
    session.connection = connection;

    // encoder output goes straight to the stream thread
    connect(sender, &StreamSender::encodedVideo, connection, [connection](const QByteArray &frame, qint64 ptsUs, bool) {
        connection->sendVideoFrame(frame, std::chrono::microseconds(ptsUs));
    });
    connect(sender, &StreamSender::encodedAudio, connection, [connection](const QByteArray &opus, qint64 ptsUs) {
        connection->sendAudioFrame(opus, std::chrono::microseconds(ptsUs));
    });
    connect(connection, &StreamConnection::keyframeRequested, sender, &StreamSender::requestKeyframe);
    connect(connection, &StreamConnection::bandwidthEstimate, sender, &StreamSender::onBandwidthEstimate);

    connect(connection, &StreamConnection::readyChanged, this, [this, connection](bool ready) {
        if (outgoing.connection != connection || !ready)
            return;
        Discord::Stream::VideoAttributes attributes;
        attributes.width = outgoing.width;
        attributes.height = outgoing.height;
        attributes.framerate = outgoing.framerate;
        attributes.maxBitrate = outgoing.maxBitrate;
        // Discord clients always flag the stream's audio SSRC as speaking (soundshare),
        // with or without captured audio
        QMetaObject::invokeMethod(connection, [connection, attributes] {
            connection->setVideoAttributes(true, attributes);
            connection->setSoundshare(true);
        });
        QMetaObject::invokeMethod(sender, [s = sender] { s->requestKeyframe(); });
        qCInfo(LogStream) << "Live:" << outgoing.key;
        emit streamingStatus(tr("Live: %1").arg(outgoing.description));
    });
    connect(connection, &StreamConnection::closed, this, [this, connection](const QString &reason) {
        if (outgoing.connection != connection)
            return;
        qCWarning(LogStream) << "Stream connection closed:" << reason;
        stopStreaming();
        emit streamingFailed(tr("Stream connection lost: %1").arg(reason));
    });

    QMetaObject::invokeMethod(connection, &StreamConnection::start);
}

void StreamManager::connectViewer(Session &session)
{
    Discord::Stream::StreamSessionInfo info;
    info.streamKey = session.key;
    info.endpoint = session.endpoint;
    info.token = session.token;
    info.rtcServerId = session.rtcServerId;
    info.selfUserId = accountId;
    info.streamerUserId = session.userId;
    info.voiceSessionId = voice->sessionId();

    auto *connection = new StreamConnection(StreamConnection::Role::Viewer, info, proxy);
    connection->moveToThread(streamThread);
    session.connection = connection;

#ifdef ACHERON_HAVE_FFMPEG
    session.decoderThread = new QThread;
    session.decoderThread->setObjectName(QStringLiteral("StreamDecodeThread"));
    session.decoderThread->start();
    auto *decoder = new StreamDecoder;
    decoder->moveToThread(session.decoderThread);
    session.decoder = decoder;

    connect(connection, &StreamConnection::videoFrame, decoder, &StreamDecoder::decode);
    connect(decoder, &StreamDecoder::needKeyframe, connection, [connection] { connection->requestKeyframe(); });
    const Snowflake userId = session.userId;
    connect(decoder, &StreamDecoder::frameDecoded, this, [this, userId](const QImage &image, const QSize &sourceSize) {
        if (watched.contains(userId))
            emit viewerFrame(userId, image, sourceSize);
    });
#endif

    connect(connection, &StreamConnection::audioPacket, this,
            [this](quint32 ssrc, quint16 sequence, quint32 timestamp, const QByteArray &opus) {
                if (voice)
                    voice->playStreamAudio(ssrc, sequence, timestamp, opus);
            });

    const Snowflake streamer = session.userId;
    connect(connection, &StreamConnection::readyChanged, this, [this, streamer](bool ready) {
        if (ready)
            emit watchConnected(streamer);
    });
    connect(connection, &StreamConnection::closed, this, [this, streamer, connection](const QString &reason) {
        auto it = watched.find(streamer);
        if (it == watched.end() || it->connection != connection)
            return;
        endWatch(streamer, tr("Connection lost: %1").arg(reason), true);
    });

    QMetaObject::invokeMethod(connection, &StreamConnection::start);
}

void StreamManager::destroyConnection(Session &session)
{
    if (session.connection) {
        StreamConnection *connection = session.connection;
        QThread *mainThread = thread();
        QMetaObject::invokeMethod(connection, [connection, mainThread] {
            connection->stop();
            connection->moveToThread(mainThread);
        }, Qt::BlockingQueuedConnection);
        delete connection;
        session.connection = nullptr;
    }
    if (session.decoderThread) {
        session.decoderThread->quit();
        session.decoderThread->wait(3000);
        delete session.decoder.data();
        session.decoder = nullptr;
        delete session.decoderThread;
        session.decoderThread = nullptr;
    }
}

} // namespace Stream
} // namespace Core
} // namespace Acheron
