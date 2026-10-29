#include "VoiceClient.hpp"
#include "VoiceGateway.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "RtpPacket.hpp"

#include "DaveSession.hpp"

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <span>

namespace Styx {
namespace Discord {
namespace Voice {

// 20ms at 48khz
static constexpr uint32_t OPUS_FRAME_SAMPLES = 960;

static std::string formatDisplayableCode(const std::vector<uint8_t> &data, int bytesToConsume = 30, int groupSize = 5)
{
    int numGroups = bytesToConsume / groupSize;
    if (static_cast<int>(data.size()) < bytesToConsume)
        return {};

    uint64_t modulus = 1;
    for (int i = 0; i < groupSize; i++)
        modulus *= 10;

    std::string result;
    for (int g = 0; g < numGroups; g++) {
        uint64_t value = 0;
        for (int b = 0; b < groupSize; b++)
            value = (value << 8) | data[g * groupSize + b];
        value %= modulus;

        if (g > 0)
            result += ' ';
        result += std::format("{:0{}}", value, groupSize);
    }
    return result;
}

VoiceClient::VoiceClient(const std::string &endpoint, const std::string &token,
                         Core::Snowflake serverId, Core::Snowflake channelId,
                         Core::Snowflake userId, const std::string &sessionId,
                         const Core::ProxyConfig &proxy, Platform::WorkerLoop *loop)
    : endpoint(endpoint),
      token(token),
      proxy(proxy),
      serverId(serverId),
      channelId(channelId),
      userId(userId),
      sessionId(sessionId),
      m_loop(loop)
{
}

VoiceClient::~VoiceClient()
{
    stop();
}

void VoiceClient::seedConnectedUsers(const std::vector<Core::Snowflake> &userIds)
{
    for (auto id : userIds)
        connectedUserIds.insert(std::to_string(id));
}

void VoiceClient::start()
{
    if (currentState != State::Disconnected) {
        Core::log(Core::LogLevel::Warning,LogVoice) << "VoiceClient::start called in non-disconnected state";
        return;
    }

    VoiceEncryption::initialize();

    setState(State::Connecting);

    gateway = std::make_unique<VoiceGateway>(endpoint, serverId, channelId, userId, sessionId, token, proxy, m_loop);

    track(gateway->connected.connect([this] { onGatewayConnected(); }));
    track(gateway->disconnected.connect([this](auto code, const auto &reason) { onGatewayDisconnected(code, reason); }));
    track(gateway->readyReceived.connect([this](const auto &data) { onGatewayReady(data); }));
    track(gateway->sessionDescriptionReceived.connect([this](const auto &data) { onSessionDescription(data); }));
    track(gateway->speakingReceived.connect([this](const auto &data) { onSpeaking(data); }));
    track(gateway->clientConnected.connect([this](const auto &data) { onClientConnect(data); }));
    track(gateway->clientsConnected.connect([this](const auto &ids) { onClientsConnect(ids); }));
    track(gateway->clientDisconnected.connect([this](auto id) { onClientDisconnect(id); }));
    track(gateway->resumed.connect([this] { onGatewayResumed(); }));
    track(gateway->binaryPayloadReceived.connect([this](int opcode, const std::string &payload) {
        if (daveSession) daveSession->handleBinaryPayload(opcode, payload);
    }));
    track(gateway->daveTransitionPrepare.connect([this](int version, int id) {
        if (daveSession) daveSession->onPrepareTransition(version, id);
    }));
    track(gateway->daveTransitionExecute.connect([this](int id) {
        if (daveSession) daveSession->onExecuteTransition(id);
    }));
    track(gateway->daveEpochPrepare.connect([this](int version, int epoch) {
        if (!daveSession && version > 0) {
            ensureDaveSession(static_cast<uint16_t>(version));
            return;
        }
        if (daveSession) daveSession->onPrepareEpoch(version, epoch);
    }));

    gateway->start();
}

void VoiceClient::stop()
{
    if (currentState == State::Disconnected)
        return;

    if (gateway) {
        gateway->hardStop();
        gateway.reset();
    }

    cleanupTransport();

    localSsrc = 0;
    selectedMode.clear();
    sessionKey.clear();

    connectedUserIds.clear();
    ssrcToUserIdMap.clear();

    setState(State::Disconnected);
}

void VoiceClient::cleanupTransport()
{
    if (m_loop && m_keepaliveTimerId) {
        m_loop->cancelTimer(m_keepaliveTimerId);
        m_keepaliveTimerId = 0;
    }

    encryption.reset();

    daveSession.reset();

    udpTransport.reset();

    rtpSequence = 0;
    rtpTimestamp = 0;
    rtpEpoch = {};
}

void VoiceClient::onGatewayConnected()
{
    Core::log(Core::LogLevel::Info,LogVoice) << "Voice gateway WebSocket connected, waiting for Hello + Identify";
    setState(State::Identifying);
}

void VoiceClient::onGatewayDisconnected(VoiceCloseCode code, const std::string &reason)
{
    Core::log(Core::LogLevel::Warning,LogVoice) << "Voice gateway disconnected, code:" << static_cast<int>(code) << "reason:" << reason;

    cleanupTransport();

    // done if not reconnected
    if (currentState != State::Disconnected) {
        setState(State::Disconnected);
        disconnected.publish();
    }
}

void VoiceClient::onGatewayReady(const VoiceReady &data)
{
    Core::log(Core::LogLevel::Info,LogVoice) << "Voice Ready: SSRC =" << data.ssrc
                     << "server =" << data.ip << ":" << data.port
                    ;

    localSsrc = data.ssrc;
    serverIp = data.ip;
    serverPort = data.port;
    serverModes = data.modes;

    static const std::array preferred = {
        EncryptionMode::AEAD_AES256_GCM_RTPSIZE,
        EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE,
    };

    EncryptionMode mode = EncryptionMode::UNKNOWN;
    for (auto candidate : preferred) {
        if (std::find(serverModes.begin(), serverModes.end(), encryptionModeToString(candidate)) != serverModes.end() && VoiceEncryption::isModeAvailable(candidate)) {
            mode = candidate;
            break;
        }
    }

    if (mode == EncryptionMode::UNKNOWN) {
        Core::log(Core::LogLevel::Error,LogVoice) << "No supported encryption mode found! Server offered:";
        stop();
        return;
    }
    selectedMode = encryptionModeToString(mode);
    Core::log(Core::LogLevel::Info,LogVoice) << "Selected encryption mode:" << selectedMode;

    setState(State::DiscoveringIP);

    cleanupTransport();

    udpTransport = std::make_unique<UdpTransport>(proxy, m_loop);
    track(udpTransport->ipDiscovered.connect([this](const auto &ip, int port) { onIpDiscovered(ip, port); }));
    track(udpTransport->ipDiscoveryFailed.connect([this](const auto &error) { onIpDiscoveryFailed(error); }));
    track(udpTransport->datagramReceived.connect([this](const auto &data) { onDatagram(data); }));

    udpTransport->startIpDiscovery(serverIp, serverPort, localSsrc);
}

void VoiceClient::onSessionDescription(const SessionDescription &desc)
{
    Core::log(Core::LogLevel::Info,LogVoice) << "Session established: mode =" << desc.mode
                     << "key length =" << desc.secretKey->size();

    sessionKey = desc.secretKey;
    selectedMode = desc.mode;

    EncryptionMode mode = encryptionModeFromString(selectedMode);
    encryption = std::make_unique<VoiceEncryption>(mode, sessionKey);

    int daveVersion = desc.daveProtocolVersion.hasValue() ? desc.daveProtocolVersion.get() : 0;
    Core::log(Core::LogLevel::Info,LogVoice) << "dave_protocol_version =" << daveVersion;

    if (daveVersion > 0)
        ensureDaveSession(static_cast<uint16_t>(daveVersion));

    rtpEpoch = std::chrono::steady_clock::now();

    setState(State::Connected);
    connected.publish();

    // send silence so discord sends us audio immediately
    sendSilence();

    if (m_loop && !m_keepaliveTimerId) {
        m_keepaliveTimerId = m_loop->setTimer(std::chrono::milliseconds(KEEPALIVE_INTERVAL_MS), true, [this] {
            sendSilence();
        });
    }
}

void VoiceClient::sendAudio(const std::string &opusData)
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    // snap rtp timestamp back to wall clock after a period of silence
    // otherwise its a little behind and it will be played back delayed by discord
    auto now = std::chrono::steady_clock::now();
    if (newTalkspurt) {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - rtpEpoch);
        rtpTimestamp = static_cast<uint32_t>(static_cast<uint64_t>(elapsed.count()) * 48 / 1000);
    } else {
        rtpTimestamp += OPUS_FRAME_SAMPLES;
    }

    RtpHeader header;
    header.payloadType = 120;
    header.marker = newTalkspurt;
    header.sequence = rtpSequence++;
    header.timestamp = rtpTimestamp;
    header.ssrc = localSsrc;

    newTalkspurt = false;

    std::string payloadForTransport = opusData;

    if (isDaveEnabled()) {
        auto *enc = daveSession->encryptor();
        auto maxSize = enc->GetMaxCiphertextByteSize(discord::dave::MediaType::Audio, payloadForTransport.size());
        std::vector<uint8_t> daveEncrypted(maxSize);
        size_t bytesWritten = 0;
        auto result = enc->Encrypt(
                discord::dave::MediaType::Audio,
                localSsrc,
                discord::dave::ArrayView<const uint8_t>(
                        reinterpret_cast<const uint8_t *>(payloadForTransport.data()),
                        payloadForTransport.size()),
                discord::dave::ArrayView<uint8_t>(daveEncrypted.data(), daveEncrypted.size()),
                &bytesWritten);

        if (result == discord::dave::IEncryptor::Success) {
            payloadForTransport = std::string(reinterpret_cast<const char *>(daveEncrypted.data()), bytesWritten);
        } else {
            Core::log(Core::LogLevel::Warning,LogVoice) << "DAVE encrypt failed: result =" << static_cast<int>(result);
            return;
        }
    }

    std::string headerBytes = header.serialize();
    std::string encryptedSection = encryption->encrypt(headerBytes, payloadForTransport);
    if (encryptedSection.empty())
        return;

    std::string packet = headerBytes + encryptedSection;
    udpTransport->send(packet);

    lastAudioSendTime = now;
}

void VoiceClient::setSpeaking(bool speaking)
{
    if (!gateway)
        return;

    if (speaking)
        newTalkspurt = true;

    int flags = speaking ? static_cast<int>(SpeakingFlag::MICROPHONE) : 0;
    gateway->sendSpeaking(flags, 0, localSsrc);
}

void VoiceClient::sendSilence()
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    // no need to keepalive by sending silence if we spoke recently
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastAudioSendTime);
    if (elapsed.count() < 5)
        return;

    std::string silencePayload(reinterpret_cast<const char *>(Core::Audio::OPUS_SILENCE), sizeof(Core::Audio::OPUS_SILENCE));
    sendAudio(silencePayload);

    Core::log(Core::LogLevel::Debug,LogVoice) << "Sent keepalive silence frame";
}

void VoiceClient::onDatagram(const std::string &data)
{
    if (data.size() < RtpHeader::FIXED_SIZE)
        return;

    const auto *p = reinterpret_cast<const uint8_t *>(data.data());

    // rtp version = 2
    if (((p[0] >> 6) & 0x03) != 2)
        return;

    // ignore all non-opus packets. theres rtcp and other stuff
    uint8_t payloadType = p[1] & 0x7F;
    if (payloadType != 120)
        return;

    // too small to contain meaningful audio after encryption overhead
    if (data.size() < 44)
        return;

    // rtp header and extension header are unencrypted and used for aad in rtpsize.
    // account for CSRC entries between fixed header and extension header.
    int csrcCount = p[0] & 0x0F;
    bool hasExtension = (p[0] >> 4) & 1;
    int headerSize = RtpHeader::FIXED_SIZE + csrcCount * 4 + (hasExtension ? 4 : 0);

    if (data.size() <= headerSize + 4)
        return;

    RtpHeader header = RtpHeader::parse(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(data.data()), data.size()));

    // ignore our own packets
    if (header.ssrc == localSsrc)
        return;

    std::string rtpHeaderBytes = data.substr(0, headerSize);
    std::string encryptedSection = data.substr(headerSize);

    if (!encryption) {
        Core::log(Core::LogLevel::Debug,LogVoice) << "Received RTP but no encryption context, SSRC =" << header.ssrc;
        return;
    }

    std::string decrypted = encryption->decrypt(rtpHeaderBytes, encryptedSection);
    if (decrypted.empty()) {
        Core::log(Core::LogLevel::Debug,LogVoice) << "Decrypt failed: SSRC =" << header.ssrc
                          << "seq =" << header.sequence
                          << "pktSize =" << data.size()
                          << "hdrSize =" << headerSize
                          << "ext =" << hasExtension
                          << "encSize =" << encryptedSection.size();
        return;
    }

    if (hasExtension) {
        int extOffset = RtpHeader::FIXED_SIZE + csrcCount * 4;
        if (data.size() < extOffset + 4)
            return;
        uint16_t extWords = (p[extOffset + 2] << 8) | p[extOffset + 3];
        int extBytes = extWords * 4;
        if (decrypted.size() <= extBytes)
            return;
        decrypted = decrypted.substr(extBytes);
    }

    if (daveSession) {
        // https://daveprotocol.com/#silence-packets
        if (decrypted.size() == sizeof(Core::Audio::OPUS_SILENCE) && memcmp(decrypted.data(), Core::Audio::OPUS_SILENCE, sizeof(Core::Audio::OPUS_SILENCE)) == 0) {
            audioReceived.publish(header.ssrc, header.sequence, header.timestamp, decrypted);
            return;
        }

        if (daveSession->isDaveEnabled()) {
            uint64_t ssrcUid = (ssrcToUserIdMap.contains(header.ssrc) ? ssrcToUserIdMap.at(header.ssrc) : 0);
            auto *dec = daveSession->getOrCreateDecryptor(header.ssrc, ssrcUid);
            auto maxSize = dec->GetMaxPlaintextByteSize(discord::dave::MediaType::Audio, decrypted.size());
            std::vector<uint8_t> davePlaintext(maxSize);
            size_t bytesWritten = 0;
            auto result = dec->Decrypt(
                    discord::dave::MediaType::Audio,
                    discord::dave::ArrayView<const uint8_t>(
                            reinterpret_cast<const uint8_t *>(decrypted.data()),
                            decrypted.size()),
                    discord::dave::ArrayView<uint8_t>(davePlaintext.data(), davePlaintext.size()),
                    &bytesWritten);

            if (result == discord::dave::IDecryptor::Success) {
                decrypted = std::string(reinterpret_cast<const char *>(davePlaintext.data()), bytesWritten);
            } else {
                static constexpr const char *kResultNames[] = {
                    "Success",
                    "DecryptionFailure",
                    "MissingKeyRatchet",
                    "InvalidNonce",
                    "MissingCryptor",
                };
                int ri = static_cast<int>(result);
                const char *rn = (ri >= 0 && ri < 5) ? kResultNames[ri] : "Unknown";
                bool hasMagic = decrypted.size() >= 2 && static_cast<uint8_t>(decrypted[decrypted.size() - 2]) == 0xFA && static_cast<uint8_t>(decrypted[decrypted.size() - 1]) == 0xFA;
                Core::log(Core::LogLevel::Debug,LogDave) << "DAVE decrypt failed: SSRC =" << header.ssrc
                                 << "result =" << rn
                                 << "frameSize =" << decrypted.size()
                                 << "hasMagicMarker =" << hasMagic;
                return;
            }
        } else if (daveSession->isDowngraded()) {
            // fallthrough
        } else {
            return;
        }
    }

    audioReceived.publish(header.ssrc, header.sequence, header.timestamp, decrypted);
}

bool VoiceClient::isDaveEnabled() const
{
    return daveSession && daveSession->isDaveEnabled();
}

void VoiceClient::requestVerificationCode(Core::Snowflake targetUserId,
                                          std::function<void(const std::string &)> callback)
{
    if (!isDaveEnabled()) {
        callback(std::string());
        return;
    }
    std::string uid = std::to_string(targetUserId);
    daveSession->getPairwiseFingerprint(uid,
                                        [cb = std::move(callback)](const std::vector<uint8_t> &fingerprint) {
                                            if (fingerprint.empty()) {
                                                cb(std::string());
                                                return;
                                            }
                                            cb(formatDisplayableCode(fingerprint, 45));
                                        });
}

void VoiceClient::onSpeaking(const SpeakingData &data)
{
    if (data.userId.hasValue() && data.userId->isValid()) {
        std::string uid = std::to_string(data.userId.get());
        connectedUserIds.insert(uid);
        if (data.ssrc.get() != 0)
            ssrcToUserIdMap[data.ssrc.get()] = static_cast<uint64_t>(data.userId.get());
        if (daveSession) {
            daveSession->addConnectedUser(uid);
            if (data.ssrc.get() != 0)
                daveSession->applyKeyRatchetForSsrc(data.ssrc, data.userId.get());
        }
    }
    speakingReceived.publish(data);
}

void VoiceClient::onClientsConnect(const std::vector<std::string> &userIds)
{
    for (const auto &id : userIds) {
        std::string uid = id;
        connectedUserIds.insert(uid);
        if (daveSession)
            daveSession->addConnectedUser(uid);
    }
}

void VoiceClient::onClientConnect(const ClientConnectData &data)
{
    if (data.userId.hasValue() && data.userId->isValid()) {
        std::string uid = std::to_string(data.userId.get());
        connectedUserIds.insert(uid);
        if (data.audioSsrc.get() != 0)
            ssrcToUserIdMap[data.audioSsrc.get()] = static_cast<uint64_t>(data.userId.get());
        if (daveSession)
            daveSession->addConnectedUser(uid);
    }
    clientConnected.publish(data);
}

void VoiceClient::onClientDisconnect(Core::Snowflake uid)
{
    std::string uidStr = std::to_string(uid);
    connectedUserIds.erase(uidStr);

    if (daveSession)
        daveSession->removeConnectedUser(uidStr);

    for (auto it = ssrcToUserIdMap.begin(); it != ssrcToUserIdMap.end(); ++it) {
        if (it->second == static_cast<uint64_t>(uid)) {
            ssrcToUserIdMap.erase(it);
            break;
        }
    }

    clientDisconnected.publish(uid);
}

void VoiceClient::onGatewayResumed()
{
    Core::log(Core::LogLevel::Info,LogVoice) << "Voice session resumed, restoring to Connected state";

    if (localSsrc == 0 || sessionKey.empty() || !encryption || !udpTransport) {
        cleanupTransport();
        if (currentState != State::Disconnected) {
            setState(State::Disconnected);
            disconnected.publish();
        }
        return;
    }

    rtpEpoch = std::chrono::steady_clock::now();
    setState(State::Connected);

    sendSilence();
    if (m_loop && !m_keepaliveTimerId) {
        m_keepaliveTimerId = m_loop->setTimer(std::chrono::milliseconds(KEEPALIVE_INTERVAL_MS), true, [this] {
            sendSilence();
        });
    }
}

void VoiceClient::onIpDiscovered(const std::string &ip, int port)
{
    Core::log(Core::LogLevel::Info,LogVoice) << "IP Discovery: external" << ip << ":" << port;

    setState(State::SelectingProtocol);

    gateway->sendSelectProtocol(ip, port, selectedMode);

    setState(State::WaitingForSession);
}

void VoiceClient::onIpDiscoveryFailed(const std::string &error)
{
    Core::log(Core::LogLevel::Error,LogVoice) << "IP Discovery failed:" << error;
    stop();
}

void VoiceClient::ensureDaveSession(uint16_t protocolVersion)
{
    if (daveSession)
        return;

    Core::log(Core::LogLevel::Info,LogVoice) << "Creating DAVE session, protocol version =" << protocolVersion;

    daveSession = std::make_unique<DaveSession>(channelId, userId, ssrcToUserIdMap);
    daveSession->setLocalSsrc(localSsrc);

    for (const auto &uid : connectedUserIds)
        daveSession->addConnectedUser(uid);

    track(daveSession->sendKeyPackage.connect([this](int op, const std::string &data) {
        if (gateway) gateway->sendBinaryPayload(op, data);
    }));
    track(daveSession->sendCommitWelcome.connect([this](int op, const std::string &data) {
        if (gateway) gateway->sendBinaryPayload(op, data);
    }));
    track(daveSession->sendReadyForTransition.connect([this](int id) {
        if (gateway) gateway->sendDaveReadyForTransition(id);
    }));
    track(daveSession->sendInvalidCommitWelcome.connect([this](int id) {
        if (gateway) gateway->sendDaveInvalidCommitWelcome(id);
    }));
    track(daveSession->daveStateChanged.connect([this](bool enabled) {
        if (enabled) {
            auto auth = daveSession->lastEpochAuthenticator();
            privacyCodeChanged.publish(auth.empty() ? std::string{} : formatDisplayableCode(auth));
        } else {
            privacyCodeChanged.publish({});
        }
    }));

    daveSession->init(protocolVersion);
}

void VoiceClient::setState(State state)
{
    if (currentState == state)
        return;

    Core::log(Core::LogLevel::Debug,LogVoice) << "VoiceClient state:" << static_cast<int>(currentState.load()) << "->" << static_cast<int>(state);
    currentState = state;
    stateChanged.publish(state);
}

} // namespace Voice
} // namespace Discord
} // namespace Styx
