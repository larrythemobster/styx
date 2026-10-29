#include "StreamConnection.hpp"

#include "StreamTransport.hpp"
#include "VideoReceiver.hpp"

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"
#include "Discord/Voice/DaveSession.hpp"
#include "Discord/Voice/VoiceGateway.hpp"

#include "Platform/Native.hpp"

#include <cstring>
#include <vector>

namespace Styx {
namespace Discord {
namespace Stream {

StreamConnection::StreamConnection(Role role, const StreamSessionInfo &info, const Core::ProxyConfig &proxy,
                                   Platform::WorkerLoop *ownerLoop)
    : connectionRole(role), info(info), proxy(proxy), ownerLoop(ownerLoop)
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
    Core::log(Core::LogLevel::Info, LogStream) << "Starting" << static_cast<int>(connectionRole)
        << "stream connection for" << info.streamKey << "endpoint" << info.endpoint;

    connectedUserIds.insert(std::to_string(static_cast<uint64_t>(info.selfUserId)));
    if (info.streamerUserId.isValid())
        connectedUserIds.insert(std::to_string(static_cast<uint64_t>(info.streamerUserId)));

    // no channel ID: a Go Live session is addressed by its rtc server alone
    gateway = std::make_unique<Voice::VoiceGateway>(info.endpoint, info.rtcServerId, Core::Snowflake::Invalid,
        info.selfUserId, info.voiceSessionId, info.token, proxy, ownerLoop);
    gateway->setVideoEnabled(true);

    track(gateway->readyReceived.connect([this](const auto &data) { onReady(data); }));
    track(gateway->sessionDescriptionReceived.connect([this](const auto &data) { onSessionDescription(data); }));
    track(gateway->videoReceived.connect([this](const auto &data) { onVideo(data); }));
    track(gateway->disconnected.connect([this](auto code, const auto &reason) { onGatewayDisconnected(code, reason); }));
    track(gateway->speakingReceived.connect([this](const Voice::SpeakingData &data) {
        if (data.userId.hasValue() && data.userId->isValid())
            mapSsrc(data.ssrc, data.userId.get());
    }));
    track(gateway->clientsConnected.connect([this](const std::vector<std::string> &userIds) {
        for (const auto &id : userIds) {
            connectedUserIds.insert(id);
            if (daveSession)
                daveSession->addConnectedUser(id);
        }
    }));
    track(gateway->clientConnected.connect([this](const Voice::ClientConnectData &data) {
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
    }));
    track(gateway->clientDisconnected.connect([this](Core::Snowflake userId) {
        const std::string uid = std::to_string(static_cast<uint64_t>(userId));
        connectedUserIds.erase(uid);
        if (daveSession)
            daveSession->removeConnectedUser(uid);
    }));
    track(gateway->resumed.connect([this] {
        Core::log(Core::LogLevel::Info, LogStream) << "Stream gateway resumed" << info.streamKey;
        sendVideoState();
    }));

    track(gateway->binaryPayloadReceived.connect([this](int opcode, const std::string &payload) {
        if (daveSession)
            daveSession->handleBinaryPayload(opcode, payload);
    }));
    track(gateway->daveTransitionPrepare.connect([this](int version, int transitionId) {
        if (daveSession)
            daveSession->onPrepareTransition(version, transitionId);
    }));
    track(gateway->daveTransitionExecute.connect([this](int transitionId) {
        if (daveSession)
            daveSession->onExecuteTransition(transitionId);
    }));
    track(gateway->daveEpochPrepare.connect([this](int version, int epoch) {
        if (!daveSession && version > 0) {
            ensureDaveSession(static_cast<uint16_t>(version));
            return;
        }
        if (daveSession)
            daveSession->onPrepareEpoch(version, epoch);
    }));

    gateway->start();
}

void StreamConnection::stop()
{
    if (stopped)
        return;
    stopped = true;

    if (gateway)
        gateway->hardStop();
    gateway.reset();
    if (transport)
        transport->close();
    transport.reset();
    receiver.reset();
    daveSession.reset();

    if (ready) {
        ready = false;
        readyChanged.publish(false);
    }
}

void StreamConnection::mapSsrc(uint32_t ssrc, Core::Snowflake userId)
{
    if (ssrc == 0 || !userId.isValid())
        return;
    ssrcToUserId[ssrc] = static_cast<uint64_t>(userId);
    if (daveSession)
        daveSession->applyKeyRatchetForSsrc(ssrc, static_cast<uint64_t>(userId));
}

void StreamConnection::onReady(const Voice::VoiceReady &data)
{
    audioSsrc = data.ssrc;
    if (data.streams.hasValue() && !data.streams->empty()) {
        const auto &layer = data.streams->front();
        videoSsrc = layer.ssrc;
        rtxSsrc = layer.rtxSsrc.valueOr(0);
    }
    Core::log(Core::LogLevel::Info, LogStream) << "Stream READY: audio ssrc" << audioSsrc << "video ssrc" << videoSsrc << "rtx ssrc" << rtxSsrc;

    if (transport)
        transport->close();
    transport = std::make_unique<StreamTransport>(connectionRole == Role::Sender
        ? StreamTransport::Role::Sender : StreamTransport::Role::Viewer, ownerLoop);
    if (connectionRole == Role::Sender) {
        transport->setLocalSsrcs(audioSsrc, videoSsrc, rtxSsrc);
        transport->setMaxBitrate(info.maxBitrate);
    } else {
        transport->setLocalSsrcs(audioSsrc, 0, 0);
    }

    track(transport->localOfferReady.connect([this](const std::string &sdp) {
        gateway->sendSelectProtocolWebRtc(sdp, StreamTransport::codecs(), Platform::randomUuid());
    }));
    track(transport->connected.connect([this] { onTransportConnected(); }));
    track(transport->failed.connect([this](const std::string &reason) {
        Core::log(Core::LogLevel::Warning, LogStream) << "Stream transport failed:" << reason;
        if (!stopped)
            closed.publish(reason);
    }));

    if (connectionRole == Role::Sender) {
        track(transport->keyframeRequested.connect([this] { keyframeRequested.publish(); }));
        track(transport->bandwidthEstimate.connect([this](unsigned int rate) { bandwidthEstimate.publish(rate); }));
    } else {
        receiver = std::make_unique<VideoReceiver>(ownerLoop);
        receiver->setLocalSsrc(audioSsrc);
        if (remoteVideoSsrc != 0)
            receiver->setSsrcs(remoteVideoSsrc, remoteRtxSsrc, remoteAudioSsrc);
        track(transport->rtpReceived.connect([this](const std::string &packet) { receiver->handleRtp(packet); }));
        track(receiver->sendRtcp.connect([this](const std::string &packet) { transport->sendRtcp(packet); }));
        track(receiver->frameReady.connect([this](const std::string &frame, uint32_t timestamp, bool keyframe) {
            std::string plain = frame;
            if (!daveDecrypt(true, remoteVideoSsrc, plain)) {
                receiver->requestKeyframe();
                return;
            }
            videoFrame.publish(plain, timestamp, keyframe);
        }));
        track(receiver->audioReady.connect([this](uint32_t ssrc, uint16_t sequence, uint32_t timestamp, const std::string &opus) {
            std::string plain = opus;
            if (!daveDecrypt(false, ssrc, plain))
                return;
            audioPacket.publish(ssrc, sequence, timestamp, plain);
        }));
    }

    // nothing is sent until the encoder starts; viewers advertise no video at all
    sendVideoState();
    transport->createOffer();
}

void StreamConnection::onSessionDescription(const Voice::SessionDescription &desc)
{
    const int daveVersion = desc.daveProtocolVersion.valueOr(0);
    Core::log(Core::LogLevel::Info, LogStream) << "Stream session description: dave version" << daveVersion
                      << "video codec" << desc.videoCodec.valueOr(std::string());

    if (!desc.sdp.hasValue() || !transport || !transport->applyServerSdp(desc.sdp.get())) {
        Core::log(Core::LogLevel::Warning, LogStream) << "Stream session description without a usable SDP";
        closed.publish("media server sent no usable SDP");
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

    Core::log(Core::LogLevel::Info, LogStream) << "Streamer media: audio" << remoteAudioSsrc << "video" << remoteVideoSsrc << "rtx" << remoteRtxSsrc;

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
    Core::log(Core::LogLevel::Info, LogStream) << "Stream transport connected:" << info.streamKey;
    if (connectionRole == Role::Viewer && receiver)
        receiver->requestKeyframe();
    if (!ready) {
        ready = true;
        readyChanged.publish(true);
    }
}

void StreamConnection::onGatewayDisconnected(Voice::VoiceCloseCode code, const std::string &reason)
{
    Core::log(Core::LogLevel::Info, LogStream) << "Stream gateway closed:" << static_cast<int>(code) << reason;
    if (!stopped)
        closed.publish(reason.empty() ? "stream connection closed" : reason);
}

void StreamConnection::ensureDaveSession(uint16_t protocolVersion)
{
    if (daveSession)
        return;

    // Go Live sessions use rtc_server_id - 1 as their MLS group
    const Core::Snowflake groupId(static_cast<uint64_t>(info.rtcServerId) - 1);
    Core::log(Core::LogLevel::Info, LogStream) << "Creating stream DAVE session, version" << protocolVersion << "group" << static_cast<uint64_t>(groupId);

    daveSession = std::make_unique<Voice::DaveSession>(groupId, info.selfUserId, ssrcToUserId);
    if (connectionRole == Role::Sender) {
        daveSession->setLocalSsrc(audioSsrc);
        daveSession->assignSsrcCodec(videoSsrc, discord::dave::Codec::H264);
    }
    for (const auto &uid : connectedUserIds)
        daveSession->addConnectedUser(uid);

    track(daveSession->sendKeyPackage.connect([this](int op, const std::string &data) { if (gateway) gateway->sendBinaryPayload(op, data); }));
    track(daveSession->sendCommitWelcome.connect([this](int op, const std::string &data) { if (gateway) gateway->sendBinaryPayload(op, data); }));
    track(daveSession->sendReadyForTransition.connect([this](int id) { if (gateway) gateway->sendDaveReadyForTransition(id); }));
    track(daveSession->sendInvalidCommitWelcome.connect([this](int id) { if (gateway) gateway->sendDaveInvalidCommitWelcome(id); }));
    track(daveSession->daveStateChanged.connect([this](bool enabled) {
        Core::log(Core::LogLevel::Info, LogStream) << "Stream DAVE" << (enabled ? "enabled" : "disabled") << info.streamKey;
        if (enabled && connectionRole == Role::Viewer && receiver)
            receiver->requestKeyframe();
        if (enabled && connectionRole == Role::Sender)
            keyframeRequested.publish();
    }));

    daveSession->init(protocolVersion);
}

void StreamConnection::sendVideoState()
{
    if (!gateway || audioSsrc == 0)
        return;

    Core::OrderedJson d;
    d.insert("audio_ssrc", static_cast<int64_t>(audioSsrc));
    Core::OrderedJson::Array streams;
    if (connectionRole == Role::Sender && videoActive) {
        d.insert("video_ssrc", static_cast<int64_t>(videoSsrc));
        d.insert("rtx_ssrc", static_cast<int64_t>(rtxSsrc));

        Core::OrderedJson resolution;
        resolution.insert("type", "fixed");
        resolution.insert("width", attributes.width);
        resolution.insert("height", attributes.height);

        Core::OrderedJson layer;
        layer.insert("type", "video");
        layer.insert("rid", "100");
        layer.insert("ssrc", static_cast<int64_t>(videoSsrc));
        layer.insert("active", true);
        layer.insert("quality", 100);
        layer.insert("rtx_ssrc", static_cast<int64_t>(rtxSsrc));
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

std::string StreamConnection::daveEncrypt(bool video, uint32_t ssrc, const std::string &frame)
{
    if (!isDaveEnabled())
        return frame;

    auto *encryptor = daveSession->encryptor();
    const auto mediaType = video ? discord::dave::MediaType::Video : discord::dave::MediaType::Audio;
    std::vector<uint8_t> out(encryptor->GetMaxCiphertextByteSize(mediaType, frame.size()));
    size_t written = 0;
    const auto result = encryptor->Encrypt(
            mediaType, ssrc,
            discord::dave::ArrayView<const uint8_t>(reinterpret_cast<const uint8_t *>(frame.data()), frame.size()),
            discord::dave::ArrayView<uint8_t>(out.data(), out.size()), &written);
    if (result != discord::dave::IEncryptor::Success) {
        Core::log(Core::LogLevel::Debug, LogStream) << "DAVE encrypt failed:" << static_cast<int>(result);
        return {};
    }
    return std::string(reinterpret_cast<const char *>(out.data()), written);
}

bool StreamConnection::daveDecrypt(bool video, uint32_t ssrc, std::string &frame)
{
    if (!daveSession)
        return true;

    // https://daveprotocol.com/#silence-packets
    if (!video && frame.size() == sizeof(Core::Audio::OPUS_SILENCE)
        && std::memcmp(frame.data(), Core::Audio::OPUS_SILENCE, sizeof(Core::Audio::OPUS_SILENCE)) == 0)
        return true;

    if (!daveSession->isDaveEnabled())
        return daveSession->isDowngraded();

    const auto it = ssrcToUserId.find(ssrc);
    auto *decryptor = daveSession->getOrCreateDecryptor(ssrc, it == ssrcToUserId.end() ? 0 : it->second);
    const auto mediaType = video ? discord::dave::MediaType::Video : discord::dave::MediaType::Audio;
    std::vector<uint8_t> out(decryptor->GetMaxPlaintextByteSize(mediaType, frame.size()));
    size_t written = 0;
    const auto result = decryptor->Decrypt(
            mediaType,
            discord::dave::ArrayView<const uint8_t>(reinterpret_cast<const uint8_t *>(frame.data()), frame.size()),
            discord::dave::ArrayView<uint8_t>(out.data(), out.size()), &written);
    if (result != discord::dave::IDecryptor::Success) {
        Core::log(Core::LogLevel::Debug, LogStream) << "DAVE decrypt failed for ssrc" << ssrc << "result" << static_cast<int>(result)
                           << "size" << frame.size();
        return false;
    }
    frame.assign(reinterpret_cast<const char *>(out.data()), written);
    return true;
}

void StreamConnection::sendVideoFrame(const std::string &annexB, std::chrono::microseconds pts)
{
    if (!transport || !ready || connectionRole != Role::Sender)
        return;
    const std::string payload = daveEncrypt(true, videoSsrc, annexB);
    if (!payload.empty())
        transport->sendVideoFrame(payload, pts);
}

void StreamConnection::sendAudioFrame(const std::string &opus, std::chrono::microseconds pts)
{
    if (!transport || !ready || connectionRole != Role::Sender)
        return;
    const std::string payload = daveEncrypt(false, audioSsrc, opus);
    if (!payload.empty())
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
