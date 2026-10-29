#include "DaveSession.hpp"
#include "VoiceEnums.hpp"
#include "Core/Logging.hpp"

namespace Styx {
namespace Discord {
namespace Voice {

DaveSession::DaveSession(Core::Snowflake channelId, Core::Snowflake userId,
                         const std::unordered_map<uint32_t, uint64_t> &ssrcMap)
    : channelId(channelId),
      userId(userId),
      ssrcMap(ssrcMap)
{
    mlsSession = discord::dave::mls::CreateSession(
            nullptr,
            "",
            [](const std::string &reason, const std::string &detail) {
                Core::log(Core::LogLevel::Warning, LogDave) << "MLS failure:" << reason
                                   << detail;
            });
}

DaveSession::~DaveSession() = default;

void DaveSession::init(uint16_t version)
{
    protocolVersion = version;
    pendingProtocolVersion = version;
    reinit();
}

void DaveSession::reinit()
{
    daveEnabled = false;
    daveDowngraded = false;

    connectedUserIds.insert(std::to_string(static_cast<uint64_t>(userId)));

    Core::log(Core::LogLevel::Info, LogDave) << "Initializing DAVE session: channel =" << channelId
                    << "protocol version =" << protocolVersion
                    << "connectedUsers =" << connectedUserIds.size();

    mlsSession->Init(
            protocolVersion,
            static_cast<uint64_t>(channelId),
            std::to_string(static_cast<uint64_t>(userId)),
            transientKey);

    daveDecryptors.clear();
    pendingTransitionReady = false;

    daveEncryptor = discord::dave::CreateEncryptor();
    if (localSsrc != 0)
        daveEncryptor->AssignSsrcToCodec(localSsrc, discord::dave::Codec::Opus);
    for (const auto &[ssrc, codec] : extraCodecs)
        daveEncryptor->AssignSsrcToCodec(ssrc, codec);

    auto keyPackage = mlsSession->GetMarshalledKeyPackage();
    if (!keyPackage.empty()) {
        std::string data(reinterpret_cast<const char *>(keyPackage.data()), keyPackage.size());
        Core::log(Core::LogLevel::Info, LogDave) << "Sending MLS key package, size =" << data.size();
        sendKeyPackage.publish(static_cast<int>(VoiceOpCode::DAVE_MLS_KEY_PACKAGE), data);
    }
}

void DaveSession::onExternalSenderPackage(const std::string &payload)
{
    Core::log(Core::LogLevel::Info, LogDave) << "Received external sender package, size =" << payload.size();

    std::vector<uint8_t> data(payload.begin(), payload.end());
    mlsSession->SetExternalSender(data);
}

void DaveSession::handleBinaryPayload(int opcode, const std::string &payload)
{
    switch (opcode) {
    case static_cast<int>(VoiceOpCode::DAVE_MLS_EXTERNAL_SENDER_PACKAGE):
        onExternalSenderPackage(payload);
        break;
    case static_cast<int>(VoiceOpCode::DAVE_MLS_PROPOSALS):
        onProposals(payload);
        break;
    case static_cast<int>(VoiceOpCode::DAVE_MLS_ANNOUNCE_COMMIT_TRANSITION):
        onAnnounceCommitTransition(payload);
        break;
    case static_cast<int>(VoiceOpCode::DAVE_MLS_WELCOME):
        onWelcome(payload);
        break;
    default:
        Core::log(Core::LogLevel::Debug, LogVoice) << "Unhandled DAVE binary opcode:" << opcode;
        break;
    }
}

void DaveSession::onProposals(const std::string &payload)
{
    Core::log(Core::LogLevel::Info, LogDave) << "Received proposals, size =" << payload.size()
                    << "connectedUsers =" << connectedUserIds.size();

    std::vector<uint8_t> data(payload.begin(), payload.end());
    auto response = mlsSession->ProcessProposals(std::move(data), connectedUserIds);

    if (response) {
        std::string commitWelcome(reinterpret_cast<const char *>(response->data()), response->size());
        Core::log(Core::LogLevel::Info, LogDave) << "Sending commit+welcome, size =" << commitWelcome.size();
        sendCommitWelcome.publish(
                static_cast<int>(VoiceOpCode::DAVE_MLS_COMMIT_WELCOME), commitWelcome);
    }
}

void DaveSession::onAnnounceCommitTransition(const std::string &payload)
{
    if (payload.size() < 2)
        return;

    const auto *raw = reinterpret_cast<const uint8_t *>(payload.data());
    int transitionId = (raw[0] << 8) | raw[1];
    pendingTransitionId = transitionId;

    Core::log(Core::LogLevel::Debug, LogDave) << "Received announce commit transition: transitionId ="
                     << transitionId << "size =" << payload.size();

    std::vector<uint8_t> data(payload.begin() + 2, payload.end());
    auto result = mlsSession->ProcessCommit(std::move(data));

    if (auto *roster = std::get_if<discord::dave::RosterMap>(&result)) {
        Core::log(Core::LogLevel::Info, LogDave) << "ProcessCommit succeeded, roster size =" << roster->size();
        pendingTransitionReady = true;
        sendReadyForTransition.publish(transitionId);
        if (transitionId == 0)
            completeTransition();
    } else if (std::holds_alternative<discord::dave::failed_t>(result)) {
        Core::log(Core::LogLevel::Warning, LogDave) << "ProcessCommit failed (hard reject)";
        sendInvalidCommitWelcome.publish(transitionId);
    } else {
        Core::log(Core::LogLevel::Debug, LogDave) << "ProcessCommit ignored (soft reject)";
    }
}

void DaveSession::onWelcome(const std::string &payload)
{
    if (payload.size() < 2)
        return;

    const auto *raw = reinterpret_cast<const uint8_t *>(payload.data());
    int transitionId = (raw[0] << 8) | raw[1];
    pendingTransitionId = transitionId;

    Core::log(Core::LogLevel::Info, LogDave) << "Received welcome: transitionId =" << transitionId
                    << "size =" << payload.size()
                    << "connectedUsers =" << connectedUserIds.size();

    std::vector<uint8_t> data(payload.begin() + 2, payload.end());
    auto roster = mlsSession->ProcessWelcome(std::move(data), connectedUserIds);

    if (roster) {
        Core::log(Core::LogLevel::Info, LogDave) << "ProcessWelcome succeeded, roster size =" << roster->size();
        pendingTransitionReady = true;
        sendReadyForTransition.publish(transitionId);
        if (transitionId == 0)
            completeTransition();
    } else {
        Core::log(Core::LogLevel::Warning, LogDave) << "ProcessWelcome failed";
        sendInvalidCommitWelcome.publish(transitionId);
    }
}

void DaveSession::onPrepareTransition(int version, int transitionId)
{
    Core::log(Core::LogLevel::Info, LogDave) << "Prepare transition: version =" << version
                    << "transitionId =" << transitionId;

    pendingTransitionId = transitionId;

    pendingProtocolVersion = static_cast<uint16_t>(version);

    sendReadyForTransition.publish(transitionId);
}

void DaveSession::onExecuteTransition(int transitionId)
{
    Core::log(Core::LogLevel::Info, LogDave) << "Execute transition: transitionId =" << transitionId;

    if (pendingProtocolVersion != protocolVersion) {
        protocolVersion = pendingProtocolVersion;
        if (protocolVersion == 0) {
            Core::log(Core::LogLevel::Info, LogDave) << "DAVE downgrade to version 0, disabling";
            daveEnabled = false;
            daveDowngraded = true;
            daveStateChanged.publish(false);
            return;
        }
    }

    if (!pendingTransitionReady) {
        Core::log(Core::LogLevel::Warning, LogDave) << "Execute transition" << transitionId
                           << "but no pending commit/welcome, reinitializing DAVE session";
        reinit();
        return;
    }

    completeTransition();
}

void DaveSession::completeTransition()
{
    if (!pendingTransitionReady) {
        Core::log(Core::LogLevel::Warning, LogDave) << "completeTransition but no pending commit/welcome!";
        return;
    }
    pendingTransitionReady = false;

    auto selfRatchet = mlsSession->GetKeyRatchet(std::to_string(userId));
    if (selfRatchet) {
        daveEncryptor->SetKeyRatchet(std::move(selfRatchet));
        Core::log(Core::LogLevel::Info, LogDave) << "Refreshed encryptor key ratchet for new epoch";
    } else {
        Core::log(Core::LogLevel::Warning, LogDave) << "Could not get own key ratchet from MLS session";
    }

    for (auto &[ssrc, dec] : daveDecryptors) {
        auto it = ssrcMap.find(ssrc);
        if (it == ssrcMap.end())
            continue;
        auto ratchet = mlsSession->GetKeyRatchet(std::to_string(it->second));
        if (ratchet) {
            dec->TransitionToKeyRatchet(std::move(ratchet));
            Core::log(Core::LogLevel::Debug, LogDave) << "Refreshed decryptor key ratchet for SSRC =" << ssrc;
        }
    }

    if (!daveEnabled) {
        daveEnabled = true;
        daveDowngraded = false;
        Core::log(Core::LogLevel::Info, LogDave) << "DAVE encryption enabled";
    }

    daveStateChanged.publish(true);

    pendingTransitionId = -1;
}

void DaveSession::onPrepareEpoch(int version, int epoch)
{
    Core::log(Core::LogLevel::Info, LogDave) << "Prepare epoch: version =" << version << "epoch =" << epoch;

    if (epoch == 1) {
        protocolVersion = static_cast<uint16_t>(version);
        reinit();
    }
}

discord::dave::IEncryptor *DaveSession::encryptor()
{
    return daveEncryptor.get();
}

discord::dave::IDecryptor *DaveSession::getOrCreateDecryptor(uint32_t ssrc, uint64_t uid)
{
    auto it = daveDecryptors.find(ssrc);
    if (it != daveDecryptors.end())
        return it->second.get();

    auto decryptor = discord::dave::CreateDecryptor();

    bool hasRatchet = false;
    if (uid != 0) {
        auto keyRatchet = mlsSession->GetKeyRatchet(std::to_string(uid));
        if (keyRatchet) {
            decryptor->TransitionToKeyRatchet(std::move(keyRatchet));
            hasRatchet = true;
        }
    }

    auto *ptr = decryptor.get();
    daveDecryptors.emplace(ssrc, std::move(decryptor));

    Core::log(Core::LogLevel::Debug, LogDave) << "Created decryptor for SSRC =" << ssrc
                     << "hasRatchet =" << hasRatchet;
    return ptr;
}

void DaveSession::setLocalSsrc(uint32_t ssrc)
{
    localSsrc = ssrc;
    if (daveEncryptor)
        daveEncryptor->AssignSsrcToCodec(ssrc, discord::dave::Codec::Opus);
}

void DaveSession::assignSsrcCodec(uint32_t ssrc, discord::dave::Codec codec)
{
    extraCodecs[ssrc] = codec;
    if (daveEncryptor)
        daveEncryptor->AssignSsrcToCodec(ssrc, codec);
}

void DaveSession::addConnectedUser(const std::string &id)
{
    connectedUserIds.insert(id);
}

void DaveSession::removeConnectedUser(const std::string &id)
{
    connectedUserIds.erase(id);

    uint64_t uid = std::stoull(id);
    for (auto it = ssrcMap.begin(); it != ssrcMap.end(); ++it) {
        if (it->second == uid) {
            daveDecryptors.erase(it->first);
            break;
        }
    }
}

void DaveSession::getPairwiseFingerprint(const std::string &userId, FingerprintCallback callback) const
{
    if (!mlsSession || !daveEnabled) {
        callback({});
        return;
    }

    mlsSession->GetPairwiseFingerprint(0, userId, std::move(callback));
}

std::vector<uint8_t> DaveSession::lastEpochAuthenticator() const
{
    if (!mlsSession || !daveEnabled)
        return {};
    return mlsSession->GetLastEpochAuthenticator();
}

void DaveSession::applyKeyRatchetForSsrc(uint32_t ssrc, uint64_t uid)
{
    auto it = daveDecryptors.find(ssrc);
    if (it == daveDecryptors.end())
        return;

    auto keyRatchet = mlsSession->GetKeyRatchet(std::to_string(uid));
    if (keyRatchet) {
        it->second->TransitionToKeyRatchet(std::move(keyRatchet));
        Core::log(Core::LogLevel::Info, LogDave) << "Applied key ratchet for SSRC =" << ssrc << "user =" << uid;
    }
}

} // namespace Voice
} // namespace Discord
} // namespace Styx
