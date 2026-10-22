#include "StreamConnection.hpp"

#include "StreamTransport.hpp"
#include "VideoReceiver.hpp"

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"
#include "Discord/Voice/DaveSession.hpp"
#include "Discord/Voice/VoiceGateway.hpp"

#include <QUuid>

#include <vector>

namespace Styx {
namespace Discord {
namespace Stream {

StreamConnection::StreamConnection(Role role, const StreamSessionInfo &info, const Core::ProxyConfig &proxy, QObject *parent)
    : QObject(parent), connectionRole(role), info(info), proxy(proxy)
{
}

StreamConnection::~StreamConnection()
{
    stop();
}

bool StreamConnection::isDaveEnabled() const
{
    return daveSession && daveSession->isDaveEnabled();
}

void StreamConnection::start()
{
    qCInfo(LogStream) << "Starting" << connectionRole << "stream connection for" << info.streamKey
                      << "endpoint" << info.endpoint;

    connectedUserIds.insert(std::to_string(static_cast<uint64_t>(info.selfUserId)));
    if (info.streamerUserId.isValid())
        connectedUserIds.insert(std::to_string(static_cast<uint64_t>(info.streamerUserId)));

    // no channel ID: a Go Live session is addressed by its rtc server alone
    gateway = new Voice::VoiceGateway(info.endpoint, info.rtcServerId, Core::Snowflake::Invalid, info.selfUserId,
                                      info.voiceSessionId, info.token, proxy, this);
    gateway->setVideoEnabled(true);

    connect(gateway, &Voice::VoiceGateway::readyReceived, this, &StreamConnection::onReady);
    connect(gateway, &Voice::VoiceGateway::sessionDescriptionReceived, this, &StreamConnection::onSessionDescription);
    connect(gateway, &Voice::VoiceGateway::videoReceived, this, &StreamConnection::onVideo);
    connect(gateway, &Voice::VoiceGateway::disconnected, this, &StreamConnection::onGatewayDisconnected);
    connect(gateway, &Voice::VoiceGateway::speakingReceived, this, [this](const Voice::SpeakingData &data) {
        if (data.userId.hasValue() && data.userId->isValid())
            mapSsrc(data.ssrc, data.userId.get());
    });
    connect(gateway, &Voice::VoiceGateway::clientsConnected, this, [this](const QStringList &userIds) {
        for (const QString &id : userIds) {
            connectedUserIds.insert(id.toStdString());
            if (daveSession)
                daveSession->addConnectedUser(id.toStdString());
        }
    });
    connect(gateway, &Voice::VoiceGateway::clientConnected, this, [this](const Voice::ClientConnectData &data) {
        if (!data.userId->isValid())
            return;
        const std::string uid = std::to_string(static_cast<uint64_t>(data.userId.get()));
        connectedUserIds.insert(uid);
        if (daveSession)
            daveSession->addConnectedUser(uid);
        if (data.audioSsrc.get() != 0)
            mapSsrc(data.audioSsrc, data.userId);
        if (data.videoSsrc.get() != 0)
            mapSsrc(data.videoSsrc, data.userId);
    });
    connect(gateway, &Voice::VoiceGateway::clientDisconnected, this, [this](Core::Snowflake userId) {
        const std::string uid = std::to_string(static_cast<uint64_t>(userId));
        connectedUserIds.erase(uid);
        if (daveSession)
            daveSession->removeConnectedUser(uid);
    });
    connect(gateway, &Voice::VoiceGateway::resumed, this, [this] {
        qCInfo(LogStream) << "Stream gateway resumed" << info.streamKey;
        sendVideoState();
    });

    connect(gateway, &Voice::VoiceGateway::binaryPayloadReceived, this, [this](int opcode, const QByteArray &payload) {
        if (daveSession)
            daveSession->handleBinaryPayload(opcode, payload);
    });
    connect(gateway, &Voice::VoiceGateway::daveTransitionPrepare, this, [this](int version, int transitionId) {
        if (daveSession)
            daveSession->onPrepareTransition(version, transitionId);
    });
    connect(gateway, &Voice::VoiceGateway::daveTransitionExecute, this, [this](int transitionId) {
        if (daveSession)
            daveSession->onExecuteTransition(transitionId);
    });
    connect(gateway, &Voice::VoiceGateway::daveEpochPrepare, this, [this](int version, int epoch) {
        if (!daveSession && version > 0) {
            ensureDaveSession(static_cast<uint16_t>(version));
            return;
        }
        if (daveSession)
            daveSession->onPrepareEpoch(version, epoch);
    });

    gateway->start();
}

void StreamConnection::stop()
{
    if (stopped)
        return;
    stopped = true;

    if (gateway) {
        gateway->hardStop();
        delete gateway;
        gateway = nullptr;
    }
    if (transport) {
        transport->close();
        delete transport;
        transport = nullptr;
    }
    delete receiver;
    receiver = nullptr;
    daveSession.reset();

    if (ready) {
        ready = false;
        emit readyChanged(false);
    }
}

void StreamConnection::mapSsrc(quint32 ssrc, Core::Snowflake userId)
{
    if (ssrc == 0 || !userId.isValid())
        return;
    ssrcToUserId.insert(ssrc, static_cast<uint64_t>(userId));
    if (daveSession)
        daveSession->applyKeyRatchetForSsrc(ssrc, static_cast<uint64_t>(userId));
}

void StreamConnection::onReady(const Voice::VoiceReady &data)
{
    audioSsrc = data.ssrc;
    if (data.streams.hasValue() && !data.streams->isEmpty()) {
        const auto &layer = data.streams->first();
        videoSsrc = layer.ssrc;
        rtxSsrc = layer.rtxSsrc.valueOr(0);
    }
    qCInfo(LogStream) << "Stream READY: audio ssrc" << audioSsrc << "video ssrc" << videoSsrc << "rtx ssrc" << rtxSsrc;

    if (transport) {
        transport->close();
        delete transport;
    }
    transport = new StreamTransport(connectionRole == Role::Sender ? StreamTransport::Role::Sender : StreamTransport::Role::Viewer, this);
    if (connectionRole == Role::Sender) {
        transport->setLocalSsrcs(audioSsrc, videoSsrc, rtxSsrc);
        transport->setMaxBitrate(info.maxBitrate);
    } else {
        transport->setLocalSsrcs(audioSsrc, 0, 0);
    }

    connect(transport, &StreamTransport::localOfferReady, this, [this](const QString &sdp) {
        gateway->sendSelectProtocolWebRtc(sdp, StreamTransport::codecs(), QUuid::createUuid().toString(QUuid::WithoutBraces));
    });
    connect(transport, &StreamTransport::connected, this, &StreamConnection::onTransportConnected);
    connect(transport, &StreamTransport::failed, this, [this](const QString &reason) {
        qCWarning(LogStream) << "Stream transport failed:" << reason;
        if (!stopped)
            emit closed(reason);
    });

    if (connectionRole == Role::Sender) {
        connect(transport, &StreamTransport::keyframeRequested, this, &StreamConnection::keyframeRequested);
        connect(transport, &StreamTransport::bandwidthEstimate, this, &StreamConnection::bandwidthEstimate);
    } else {
        delete receiver;
        receiver = new VideoReceiver(this);
        receiver->setLocalSsrc(audioSsrc);
        if (remoteVideoSsrc != 0)
            receiver->setSsrcs(remoteVideoSsrc, remoteRtxSsrc, remoteAudioSsrc);
        connect(transport, &StreamTransport::rtpReceived, receiver, &VideoReceiver::handleRtp);
        connect(receiver, &VideoReceiver::sendRtcp, transport, &StreamTransport::sendRtcp);
        connect(receiver, &VideoReceiver::frameReady, this, [this](const QByteArray &frame, quint32 timestamp, bool keyframe) {
            QByteArray plain = frame;
            if (!daveDecrypt(true, remoteVideoSsrc, plain)) {
                receiver->requestKeyframe();
                return;
            }
            emit videoFrame(plain, timestamp, keyframe);
        });
        connect(receiver, &VideoReceiver::audioReady, this,
                [this](quint32 ssrc, quint16 sequence, quint32 timestamp, const QByteArray &opus) {
                    QByteArray plain = opus;
                    if (!daveDecrypt(false, ssrc, plain))
                        return;
                    emit audioPacket(ssrc, sequence, timestamp, plain);
                });
    }

    // nothing is sent until the encoder starts; viewers advertise no video at all
    sendVideoState();
    transport->createOffer();
}

void StreamConnection::onSessionDescription(const Voice::SessionDescription &desc)
{
    const int daveVersion = desc.daveProtocolVersion.valueOr(0);
    qCInfo(LogStream) << "Stream session description: dave version" << daveVersion
                      << "video codec" << desc.videoCodec.valueOr(QString());

    if (!desc.sdp.hasValue() || !transport || !transport->applyServerSdp(desc.sdp.get())) {
        qCWarning(LogStream) << "Stream session description without a usable SDP";
        emit closed(QStringLiteral("media server sent no usable SDP"));
        return;
    }

    if (daveVersion > 0)
        ensureDaveSession(static_cast<uint16_t>(daveVersion));
}

void StreamConnection::onVideo(const Voice::VideoData &data)
{
    if (connectionRole != Role::Viewer)
        return;
    if (data.userId.hasValue() && info.streamerUserId.isValid() && data.userId.get() != info.streamerUserId)
        return;

    remoteAudioSsrc = data.audioSsrc;
    remoteVideoSsrc = data.videoSsrc;
    remoteRtxSsrc = data.rtxSsrc.valueOr(0);
    if (data.streams.hasValue()) {
        for (const auto &layer : data.streams.get()) {
            if (layer.ssrc.get() != 0 && layer.active.valueOr(true)) {
                remoteVideoSsrc = layer.ssrc;
                remoteRtxSsrc = layer.rtxSsrc.valueOr(remoteRtxSsrc);
                break;
            }
        }
    }

    qCInfo(LogStream) << "Streamer media: audio" << remoteAudioSsrc << "video" << remoteVideoSsrc << "rtx" << remoteRtxSsrc;

    const Core::Snowflake streamer = data.userId.hasValue() ? data.userId.get() : info.streamerUserId;
    mapSsrc(remoteAudioSsrc, streamer);
    mapSsrc(remoteVideoSsrc, streamer);

    if (receiver) {
        receiver->setSsrcs(remoteVideoSsrc, remoteRtxSsrc, remoteAudioSsrc);
        receiver->requestKeyframe();
    }
}

void StreamConnection::onTransportConnected()
{
    qCInfo(LogStream) << "Stream transport connected:" << info.streamKey;
    if (connectionRole == Role::Viewer && receiver)
        receiver->requestKeyframe();
    if (!ready) {
        ready = true;
        emit readyChanged(true);
    }
}

void StreamConnection::onGatewayDisconnected(Voice::VoiceCloseCode code, const QString &reason)
{
    qCInfo(LogStream) << "Stream gateway closed:" << code << reason;
    if (!stopped)
        emit closed(reason.isEmpty() ? QStringLiteral("stream connection closed") : reason);
}

void StreamConnection::ensureDaveSession(uint16_t protocolVersion)
{
    if (daveSession)
        return;

    // Go Live sessions use rtc_server_id - 1 as their MLS group
    const Core::Snowflake groupId(static_cast<uint64_t>(info.rtcServerId) - 1);
    qCInfo(LogStream) << "Creating stream DAVE session, version" << protocolVersion << "group" << groupId;

    daveSession = std::make_unique<Voice::DaveSession>(groupId, info.selfUserId, ssrcToUserId, this);
    if (connectionRole == Role::Sender) {
        daveSession->setLocalSsrc(audioSsrc);
        daveSession->assignSsrcCodec(videoSsrc, discord::dave::Codec::H264);
    }
    for (const auto &uid : connectedUserIds)
        daveSession->addConnectedUser(uid);

    connect(daveSession.get(), &Voice::DaveSession::sendKeyPackage, gateway, &Voice::VoiceGateway::sendBinaryPayload);
    connect(daveSession.get(), &Voice::DaveSession::sendCommitWelcome, gateway, &Voice::VoiceGateway::sendBinaryPayload);
    connect(daveSession.get(), &Voice::DaveSession::sendReadyForTransition, gateway, &Voice::VoiceGateway::sendDaveReadyForTransition);
    connect(daveSession.get(), &Voice::DaveSession::sendInvalidCommitWelcome, gateway, &Voice::VoiceGateway::sendDaveInvalidCommitWelcome);
    connect(daveSession.get(), &Voice::DaveSession::daveStateChanged, this, [this](bool enabled) {
        qCInfo(LogStream) << "Stream DAVE" << (enabled ? "enabled" : "disabled") << info.streamKey;
        // decryptors were rebuilt; ask for a keyframe so the picture recovers immediately
        if (enabled && connectionRole == Role::Viewer && receiver)
            receiver->requestKeyframe();
        if (enabled && connectionRole == Role::Sender)
            emit keyframeRequested();
    });

    daveSession->init(protocolVersion);
}

void StreamConnection::sendVideoState()
{
    if (!gateway || audioSsrc == 0)
        return;

    Core::OrderedJson d;
    d.insert("audio_ssrc", static_cast<qint64>(audioSsrc));
    Core::OrderedJson::Array streams;
    if (connectionRole == Role::Sender && videoActive) {
        d.insert("video_ssrc", static_cast<qint64>(videoSsrc));
        d.insert("rtx_ssrc", static_cast<qint64>(rtxSsrc));

        Core::OrderedJson resolution;
        resolution.insert("type", QStringLiteral("fixed"));
        resolution.insert("width", attributes.width);
        resolution.insert("height", attributes.height);

        Core::OrderedJson layer;
        layer.insert("type", QStringLiteral("video"));
        layer.insert("rid", QStringLiteral("100"));
        layer.insert("ssrc", static_cast<qint64>(videoSsrc));
        layer.insert("active", true);
        layer.insert("quality", 100);
        layer.insert("rtx_ssrc", static_cast<qint64>(rtxSsrc));
        layer.insert("max_bitrate", attributes.maxBitrate);
        layer.insert("max_framerate", attributes.framerate);
        layer.insert("max_resolution", resolution);
        streams.append(layer);
    } else {
        d.insert("video_ssrc", 0);
        d.insert("rtx_ssrc", 0);
    }
    d.insert("streams", streams);
    gateway->sendVideo(d);
}

void StreamConnection::setVideoAttributes(bool active, const VideoAttributes &attrs)
{
    videoActive = active;
    attributes = attrs;
    sendVideoState();
}

void StreamConnection::setSoundshare(bool active)
{
    if (gateway && audioSsrc != 0)
        gateway->sendSpeaking(active ? static_cast<int>(Voice::SpeakingFlag::SOUNDSHARE) : 0, 0, audioSsrc);
}

QByteArray StreamConnection::daveEncrypt(bool video, quint32 ssrc, const QByteArray &frame)
{
    if (!isDaveEnabled())
        return frame;

    auto *encryptor = daveSession->encryptor();
    const auto mediaType = video ? discord::dave::MediaType::Video : discord::dave::MediaType::Audio;
    std::vector<uint8_t> out(encryptor->GetMaxCiphertextByteSize(mediaType, frame.size()));
    size_t written = 0;
    const auto result = encryptor->Encrypt(
            mediaType, ssrc,
            discord::dave::ArrayView<const uint8_t>(reinterpret_cast<const uint8_t *>(frame.constData()), frame.size()),
            discord::dave::ArrayView<uint8_t>(out.data(), out.size()), &written);
    if (result != discord::dave::IEncryptor::Success) {
        qCDebug(LogStream) << "DAVE encrypt failed:" << static_cast<int>(result);
        return {};
    }
    return QByteArray(reinterpret_cast<const char *>(out.data()), static_cast<qsizetype>(written));
}

bool StreamConnection::daveDecrypt(bool video, quint32 ssrc, QByteArray &frame)
{
    if (!daveSession)
        return true;

    // https://daveprotocol.com/#silence-packets
    if (!video && frame.size() == sizeof(Core::Audio::OPUS_SILENCE)
        && memcmp(frame.constData(), Core::Audio::OPUS_SILENCE, sizeof(Core::Audio::OPUS_SILENCE)) == 0)
        return true;

    if (!daveSession->isDaveEnabled())
        return daveSession->isDowngraded();

    auto *decryptor = daveSession->getOrCreateDecryptor(ssrc, ssrcToUserId.value(ssrc, 0));
    const auto mediaType = video ? discord::dave::MediaType::Video : discord::dave::MediaType::Audio;
    std::vector<uint8_t> out(decryptor->GetMaxPlaintextByteSize(mediaType, frame.size()));
    size_t written = 0;
    const auto result = decryptor->Decrypt(
            mediaType,
            discord::dave::ArrayView<const uint8_t>(reinterpret_cast<const uint8_t *>(frame.constData()), frame.size()),
            discord::dave::ArrayView<uint8_t>(out.data(), out.size()), &written);
    if (result != discord::dave::IDecryptor::Success) {
        qCDebug(LogStream) << "DAVE decrypt failed for ssrc" << ssrc << "result" << static_cast<int>(result)
                           << "size" << frame.size();
        return false;
    }
    frame = QByteArray(reinterpret_cast<const char *>(out.data()), static_cast<qsizetype>(written));
    return true;
}

void StreamConnection::sendVideoFrame(const QByteArray &annexB, std::chrono::microseconds pts)
{
    if (!transport || !ready || connectionRole != Role::Sender)
        return;
    const QByteArray payload = daveEncrypt(true, videoSsrc, annexB);
    if (!payload.isEmpty())
        transport->sendVideoFrame(payload, pts);
}

void StreamConnection::sendAudioFrame(const QByteArray &opus, std::chrono::microseconds pts)
{
    if (!transport || !ready || connectionRole != Role::Sender)
        return;
    const QByteArray payload = daveEncrypt(false, audioSsrc, opus);
    if (!payload.isEmpty())
        transport->sendAudioFrame(payload, pts);
}

void StreamConnection::requestKeyframe()
{
    if (receiver)
        receiver->requestKeyframe();
}

} // namespace Stream
} // namespace Discord
} // namespace Styx
