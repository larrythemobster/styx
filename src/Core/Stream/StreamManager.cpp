#include "StreamManager.hpp"

#include "StreamDecoder.hpp"
#include "StreamSender.hpp"
#include "Core/Audio/VoiceManager.hpp"
#include "Core/Logging.hpp"
#include "Discord/Client.hpp"
#include "Discord/Stream/StreamConnection.hpp"
#include "Platform/Dispatcher.hpp"

#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <utility>
#include <vector>

namespace Styx::Core::Stream {

using Discord::Stream::StreamConnection;

StreamManager::StreamManager(Snowflake accountId, const ProxyConfig &proxy, Discord::Client *client,
                             Audio::VoiceManager *voice)
    : accountId(accountId), proxy(proxy), client(client), voice(voice)
{
}

StreamManager::~StreamManager()
{
    stopAll();
    if (sender && senderLoop && senderLoop->isRunning()) {
        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future();
        senderLoop->post([sender = std::move(sender), done]() mutable {
            sender->stop();
            sender.reset();
            done->set_value();
        });
        finished.wait();
    }
    if (senderLoop)
        senderLoop->stopAndWait();
    if (streamLoop)
        streamLoop->stopAndWait();
}

bool StreamManager::canWatch(std::string *reason) const
{
    auto fail = [reason](const char *text) {
        if (reason)
            *reason = text;
        return false;
    };
    if (!voice || !voice->isConnected() || voice->sessionId().empty())
        return fail("Join a voice channel first.");
    if (proxy.enabled())
        return fail("Go Live does not work through a proxy: the stream would bypass it.");
    return true;
}

bool StreamManager::canStream(std::string *reason) const
{
    if (!canWatch(reason))
        return false;
    if (!isScreenCaptureSupported()) {
        if (reason)
            *reason = "Screen capture needs Windows 10 version 1903 or later.";
        return false;
    }
    return true;
}

std::string StreamManager::streamKeyFor(Snowflake userId) const
{
    const Snowflake guild = voice ? voice->currentGuildId() : Snowflake::Invalid;
    const Snowflake channel = voice ? voice->currentChannelId() : Snowflake::Invalid;
    if (guild.isValid())
        return "guild:" + guild.toString() + ":" + channel.toString() + ":" + userId.toString();
    return "call:" + channel.toString() + ":" + userId.toString();
}

void StreamManager::ensureStreamLoop()
{
    if (!streamLoop)
        streamLoop = std::make_unique<Platform::WorkerLoop>("StreamThread");
}

void StreamManager::ensureSenderLoop()
{
    if (senderLoop)
        return;
    Platform::WorkerOptions opts;
    opts.name = "StreamCaptureThread";
    opts.initComMta = true;
    senderLoop = std::make_unique<Platform::WorkerLoop>(opts);
    sender = std::make_shared<StreamSender>(*senderLoop);
    Platform::Guard<StreamManager> self(this);
    track(sender->started.connect([self](int width, int height, int framerate, int bitrate, bool audio,
                                        const std::string &encoderName) {
        Platform::Dispatcher::main().post([self, width, height, framerate, bitrate, audio, encoderName] {
            auto *manager = self.get();
            if (!manager || !manager->outgoing.active)
                return;
            manager->senderStarted = true;
            auto &outgoing = manager->outgoing;
            outgoing.width = width;
            outgoing.height = height;
            outgoing.framerate = framerate;
            outgoing.maxBitrate = bitrate;
            outgoing.audio = audio;
            outgoing.description = std::to_string(width) + "×" + std::to_string(height) + " at "
                + std::to_string(framerate) + " fps, " + encoderName + (audio ? ", with audio" : "");
            log(LogLevel::Info, LogStream) << "Capture running:" << outgoing.description << "- creating stream" << outgoing.key;
            if (manager->client && manager->voice) {
                manager->client->sendStreamCreate(manager->voice->currentGuildId(), manager->voice->currentChannelId());
                manager->client->sendStreamSetPaused(outgoing.key, false);
            }
            manager->streamingStatus.publish("Connecting…");
        });
    }));
    track(sender->failed.connect([self](const std::string &reason) {
        Platform::Dispatcher::main().post([self, reason] {
            if (auto *manager = self.get()) {
                log(LogLevel::Warning, LogStream) << "Stream sender failed:" << reason;
                manager->stopStreaming();
                manager->streamingFailed.publish(reason);
            }
        });
    }));
    track(sender->sourceClosed.connect([self] {
        Platform::Dispatcher::main().post([self] {
            if (auto *manager = self.get()) {
                log(LogLevel::Info, LogStream) << "Shared window closed, ending stream";
                manager->stopStreaming();
                manager->streamingFailed.publish("The shared window was closed.");
            }
        });
    }));
}

void StreamManager::startStreaming(const CaptureSource &source, const StreamQuality &quality, bool includeAudio)
{
    std::string reason;
    if (!canStream(&reason)) {
        streamingFailed.publish(reason);
        return;
    }
    if (outgoing.active)
        stopStreaming();
    ensureStreamLoop();
    ensureSenderLoop();
    outgoing = Session{};
    outgoing.key = streamKeyFor(accountId);
    outgoing.userId = accountId;
    outgoing.active = true;
    senderStarted = false;
    streamingChanged.publish(true);
    streamingStatus.publish("Starting capture…");
    senderLoop->post([sender = sender, source, quality, includeAudio] { sender->start(source, quality, includeAudio); });
}

void StreamManager::stopStreaming()
{
    if (!outgoing.active)
        return;
    log(LogLevel::Info, LogStream) << "Stopping stream" << outgoing.key;
    if (senderStarted && client)
        client->sendStreamDelete(outgoing.key);
    destroyConnection(outgoing);
    if (sender && senderLoop)
        senderLoop->post([sender = sender] { sender->stop(); });
    outgoing = Session{};
    senderStarted = false;
    streamingChanged.publish(false);
}

void StreamManager::watchStream(Snowflake userId)
{
    std::string reason;
    if (!canWatch(&reason)) {
        watchEnded.publish(userId, reason);
        return;
    }
    if (watched.contains(userId) || userId == accountId)
        return;
    ensureStreamLoop();
    Session session;
    session.key = streamKeyFor(userId);
    session.userId = userId;
    session.active = true;
    log(LogLevel::Info, LogStream) << "Watching stream" << session.key;
    if (client)
        client->sendStreamWatch(session.key);
    watched.emplace(userId, std::move(session));
    watchStarted.publish(userId);
}

void StreamManager::stopWatching(Snowflake userId)
{
    endWatch(userId, {}, true);
}

void StreamManager::endWatch(Snowflake userId, const std::string &reason, bool notifyServer)
{
    auto it = watched.find(userId);
    if (it == watched.end())
        return;
    Session session = std::move(it->second);
    watched.erase(it);
    log(LogLevel::Info, LogStream) << "Leaving stream" << session.key << reason;
    if (notifyServer && client)
        client->sendStreamDelete(session.key);
    destroyConnection(session);
    watchEnded.publish(userId, reason);
}

void StreamManager::setViewerSize(Snowflake userId, const Platform::Size &size)
{
#ifdef STYX_HAVE_FFMPEG
    auto it = watched.find(userId);
    if (it != watched.end() && it->second.decoder)
        it->second.decoder->setTargetSize(size);
#endif
}

void StreamManager::requestViewerKeyframe(Snowflake userId)
{
    auto it = watched.find(userId);
    if (it != watched.end() && it->second.connection && streamLoop) {
        std::weak_ptr<StreamConnection> connection = it->second.connection;
        streamLoop->post([connection] { if (auto c = connection.lock()) c->requestKeyframe(); });
    }
}

void StreamManager::stopAll()
{
    stopStreaming();
    while (!watched.empty())
        endWatch(watched.begin()->first, "You left the voice channel.", false);
}

void StreamManager::handleStreamCreate(const Discord::StreamCreate &event)
{
    const auto &key = event.streamKey.get();
    Session *session = nullptr;
    bool isSender = false;
    if (outgoing.active && outgoing.key == key) {
        session = &outgoing;
        isSender = true;
    } else {
        for (auto &[user, candidate] : watched) {
            if (candidate.key == key) { session = &candidate; break; }
        }
    }
    if (!session)
        return;
    log(LogLevel::Info, LogStream) << "STREAM_CREATE" << key << "rtc server" << event.rtcServerId.get().toString();
    if (session->connection && session->rtcServerId != event.rtcServerId.get())
        destroyConnection(*session);
    session->rtcServerId = event.rtcServerId.get();
    maybeConnect(*session, isSender);
}

void StreamManager::handleStreamServerUpdate(const Discord::StreamServerUpdate &event)
{
    const auto &key = event.streamKey.get();
    Session *session = nullptr;
    bool isSender = false;
    if (outgoing.active && outgoing.key == key) {
        session = &outgoing;
        isSender = true;
    } else {
        for (auto &[user, candidate] : watched) {
            if (candidate.key == key) { session = &candidate; break; }
        }
    }
    if (!session)
        return;
    if (event.endpoint.isNull() || event.endpoint.get().empty()) {
        log(LogLevel::Info, LogStream) << "STREAM_SERVER_UPDATE without endpoint for" << key << "- waiting";
        return;
    }
    log(LogLevel::Info, LogStream) << "STREAM_SERVER_UPDATE" << key << "endpoint" << event.endpoint.get();
    if (session->connection)
        destroyConnection(*session);
    session->endpoint = event.endpoint.get();
    session->token = event.token.get();
    maybeConnect(*session, isSender);
}

void StreamManager::handleStreamDelete(const Discord::StreamDelete &event)
{
    const auto &key = event.streamKey.get();
    const std::string reason = event.reason.hasValue() && !event.reason.isNull() ? event.reason.get() : std::string{};
    log(LogLevel::Info, LogStream) << "STREAM_DELETE" << key << reason;
    if (outgoing.active && outgoing.key == key) {
        senderStarted = false;
        stopStreaming();
        if (!reason.empty() && reason != "user_requested")
            streamingFailed.publish("Discord ended the stream (" + reason + ").");
        return;
    }
    for (auto it = watched.begin(); it != watched.end(); ++it) {
        if (it->second.key == key) {
            const auto userId = it->first;
            endWatch(userId, reason == "stream_ended" || reason.empty() ? "The stream ended."
                     : "Stream closed (" + reason + ").", false);
            return;
        }
    }
}

void StreamManager::maybeConnect(Session &session, bool isSender)
{
    if (session.connection || !session.rtcServerId.isValid() || session.endpoint.empty() || !voice || voice->sessionId().empty())
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
    auto connection = std::make_shared<StreamConnection>(StreamConnection::Role::Sender, info, proxy, streamLoop.get());
    session.connection = connection;
    std::weak_ptr<StreamConnection> weakConnection = connection;
    std::weak_ptr<StreamSender> weakSender = sender;
    auto *streamWorker = streamLoop.get();
    auto *senderWorker = senderLoop.get();
    Platform::Guard<StreamManager> self(this);

    connection->track(sender->encodedVideo.connect([weakConnection, streamWorker](const std::string &frame, int64_t pts, bool) {
        streamWorker->post([weakConnection, frame, pts] { if (auto c = weakConnection.lock()) c->sendVideoFrame(frame, std::chrono::microseconds(pts)); });
    }));
    connection->track(sender->encodedAudio.connect([weakConnection, streamWorker](const std::string &opus, int64_t pts) {
        streamWorker->post([weakConnection, opus, pts] { if (auto c = weakConnection.lock()) c->sendAudioFrame(opus, std::chrono::microseconds(pts)); });
    }));
    connection->track(connection->keyframeRequested.connect([weakSender, senderWorker] {
        senderWorker->post([weakSender] { if (auto s = weakSender.lock()) s->requestKeyframe(); });
    }));
    connection->track(connection->bandwidthEstimate.connect([weakSender, senderWorker](unsigned int bitrate) {
        senderWorker->post([weakSender, bitrate] { if (auto s = weakSender.lock()) s->onBandwidthEstimate(bitrate); });
    }));
    connection->track(connection->readyChanged.connect([self, weakConnection](bool ready) {
        Platform::Dispatcher::main().post([self, weakConnection, ready] {
            auto *manager = self.get();
            if (!manager || !manager->outgoing.connection
                || manager->outgoing.connection != weakConnection.lock() || !ready)
                return;
            Discord::Stream::VideoAttributes attributes;
            attributes.width = manager->outgoing.width;
            attributes.height = manager->outgoing.height;
            attributes.framerate = manager->outgoing.framerate;
            attributes.maxBitrate = manager->outgoing.maxBitrate;
            manager->streamLoop->post([weakConnection, attributes] {
                if (auto c = weakConnection.lock()) { c->setVideoAttributes(true, attributes); c->setSoundshare(true); }
            });
            if (manager->sender && manager->senderLoop)
                manager->senderLoop->post([sender = manager->sender] { sender->requestKeyframe(); });
            log(LogLevel::Info, LogStream) << "Live:" << manager->outgoing.key;
            manager->streamingStatus.publish("Live: " + manager->outgoing.description);
        });
    }));
    connection->track(connection->closed.connect([self, weakConnection](const std::string &reason) {
        Platform::Dispatcher::main().post([self, weakConnection, reason] {
            auto *manager = self.get();
            if (!manager || !manager->outgoing.connection
                || manager->outgoing.connection != weakConnection.lock())
                return;
            log(LogLevel::Warning, LogStream) << "Stream connection closed:" << reason;
            manager->stopStreaming();
            manager->streamingFailed.publish("Stream connection lost: " + reason);
        });
    }));
    streamLoop->post([weakConnection] { if (auto c = weakConnection.lock()) c->start(); });
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
    auto connection = std::make_shared<StreamConnection>(StreamConnection::Role::Viewer, info, proxy, streamLoop.get());
    session.connection = connection;
    std::weak_ptr<StreamConnection> weakConnection = connection;
    Platform::Guard<StreamManager> self(this);
    const Snowflake streamer = session.userId;
#ifdef STYX_HAVE_FFMPEG
    session.decoderLoop = std::make_shared<Platform::WorkerLoop>("StreamDecodeThread");
    session.decoder = std::make_shared<StreamDecoder>();
    std::weak_ptr<StreamDecoder> weakDecoder = session.decoder;
    auto decoderLoop = session.decoderLoop;
    auto *streamWorker = streamLoop.get();
    struct DecodeQueue {
        struct Frame { std::string bytes; uint32_t timestamp; bool keyframe; };
        std::mutex mutex;
        std::deque<Frame> frames;
        bool scheduled = false;
        bool waitingForKeyframe = true;
    };
    auto queue = std::make_shared<DecodeQueue>();
    connection->track(connection->videoFrame.connect([weakDecoder, decoderLoop, weakConnection, streamWorker, queue](const std::string &frame, uint32_t timestamp, bool keyframe) {
        bool schedule = false;
        bool requestKeyframe = false;
        {
            std::lock_guard lock(queue->mutex);
            if (queue->waitingForKeyframe && !keyframe)
                return;
            if (keyframe) {
                queue->frames.clear();
                queue->waitingForKeyframe = false;
            }
            if (queue->frames.size() >= 3) {
                // Predictive pictures cannot be dropped individually without losing references.
                queue->frames.clear();
                queue->waitingForKeyframe = true;
                requestKeyframe = true;
            } else {
                queue->frames.push_back({frame, timestamp, keyframe});
                if (!queue->scheduled) {
                    queue->scheduled = true;
                    schedule = true;
                }
            }
        }
        if (requestKeyframe) {
            streamWorker->post([weakConnection] { if (auto connection = weakConnection.lock()) connection->requestKeyframe(); });
        }
        if (schedule) {
            decoderLoop->post([weakDecoder, queue] {
                for (;;) {
                    DecodeQueue::Frame frame;
                    {
                        std::lock_guard lock(queue->mutex);
                        if (queue->frames.empty()) {
                            queue->scheduled = false;
                            return;
                        }
                        frame = std::move(queue->frames.front());
                        queue->frames.pop_front();
                    }
                    if (auto decoder = weakDecoder.lock())
                        decoder->decode(frame.bytes, frame.timestamp, frame.keyframe);
                }
            });
        }
    }));
    connection->track(session.decoder->needKeyframe.connect([weakConnection, streamWorker, queue] {
        {
            std::lock_guard lock(queue->mutex);
            queue->frames.clear();
            queue->waitingForKeyframe = true;
        }
        streamWorker->post([weakConnection] { if (auto c = weakConnection.lock()) c->requestKeyframe(); });
    }));
    struct FrameDelivery {
        std::mutex mutex;
        Platform::ImageBuffer image;
        Platform::Size size;
        bool queued = false;
    };
    auto delivery = std::make_shared<FrameDelivery>();
    session.decoder->onFrameDecoded = [self, streamer, weakConnection, delivery](Platform::ImageBuffer image, Platform::Size size) {
        {
            std::lock_guard lock(delivery->mutex);
            delivery->image = std::move(image);
            delivery->size = size;
            if (delivery->queued)
                return;
            delivery->queued = true;
        }
        Platform::Dispatcher::main().post([self, streamer, weakConnection, delivery] {
            Platform::ImageBuffer image;
            Platform::Size size;
            {
                std::lock_guard lock(delivery->mutex);
                image = std::move(delivery->image);
                size = delivery->size;
                delivery->queued = false;
            }
            if (auto *manager = self.get()) {
                auto it = manager->watched.find(streamer);
                if (it != manager->watched.end() && it->second.connection == weakConnection.lock())
                    manager->viewerFrame.publish(streamer, image, size);
            }
        });
    };
#endif
    connection->track(connection->audioPacket.connect([self, streamer, weakConnection](uint32_t ssrc, uint16_t sequence, uint32_t timestamp, const std::string &opus) {
        Platform::Dispatcher::main().post([self, streamer, weakConnection, ssrc, sequence, timestamp, opus] {
            if (auto *manager = self.get()) {
                auto it = manager->watched.find(streamer);
                if (it != manager->watched.end() && it->second.connection == weakConnection.lock() && manager->voice)
                    manager->voice->playStreamAudio(ssrc, sequence, timestamp, opus);
            }
        });
    }));
    connection->track(connection->readyChanged.connect([self, streamer, weakConnection](bool ready) {
        Platform::Dispatcher::main().post([self, streamer, weakConnection, ready] {
            if (auto *manager = self.get()) {
                auto it = manager->watched.find(streamer);
                if (ready && it != manager->watched.end() && it->second.connection == weakConnection.lock())
                    manager->watchConnected.publish(streamer);
            }
        });
    }));
    connection->track(connection->closed.connect([self, streamer, weakConnection](const std::string &reason) {
        Platform::Dispatcher::main().post([self, streamer, weakConnection, reason] {
            if (auto *manager = self.get()) {
                auto it = manager->watched.find(streamer);
                if (it != manager->watched.end() && it->second.connection == weakConnection.lock())
                    manager->endWatch(streamer, "Connection lost: " + reason, true);
            }
        });
    }));
    streamLoop->post([weakConnection] { if (auto c = weakConnection.lock()) c->start(); });
}

void StreamManager::destroyConnection(Session &session)
{
    if (session.connection) {
        auto connection = std::move(session.connection);
        if (streamLoop && streamLoop->isRunning()) {
            auto done = std::make_shared<std::promise<void>>();
            auto finished = done->get_future();
            streamLoop->post([connection = std::move(connection), done]() mutable {
                connection->stop();
                connection.reset();
                done->set_value();
            });
            finished.wait();
        } else {
            connection->stop();
        }
    }
    if (session.decoderLoop) {
        session.decoderLoop->stopAndWait();
        session.decoder.reset();
        session.decoderLoop.reset();
    }
}

} // namespace Styx::Core::Stream
