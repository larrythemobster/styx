#pragma once

#include <cstdint>
#include "Platform/Trackable.hpp"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <dave/dave_interfaces.h>

#include "Core/Snowflake.hpp"

namespace mlspp {
struct SignaturePrivateKey;
}

namespace Styx {
namespace Discord {
namespace Voice {

class DaveSession : public Platform::Trackable
{
public:
    // groupId is the MLS group: the channel ID for voice, rtc_server_id - 1 for Go Live.
    DaveSession(Core::Snowflake groupId, Core::Snowflake userId,
                const std::unordered_map<uint32_t, uint64_t> &ssrcMap);
    ~DaveSession() override;

    void init(uint16_t protocolVersion);

    // binary
    void handleBinaryPayload(int opcode, const std::string &payload);
    void onExternalSenderPackage(const std::string &payload);
    void onProposals(const std::string &payload);
    void onAnnounceCommitTransition(const std::string &payload);
    void onWelcome(const std::string &payload);

    // json
    void onPrepareTransition(int protocolVersion, int transitionId);
    void onExecuteTransition(int transitionId);
    void onPrepareEpoch(int protocolVersion, int epoch);

    discord::dave::IEncryptor *encryptor();
    discord::dave::IDecryptor *getOrCreateDecryptor(uint32_t ssrc, uint64_t userId);

    bool isDaveEnabled() const { return daveEnabled; }

    bool isDowngraded() const { return daveDowngraded; }

    using FingerprintCallback = std::function<void(const std::vector<uint8_t> &)>;
    void getPairwiseFingerprint(const std::string &userId, FingerprintCallback callback) const;

    std::vector<uint8_t> lastEpochAuthenticator() const;

    void setLocalSsrc(uint32_t ssrc);
    // Codec for a sending SSRC other than the Opus one; survives reinit().
    void assignSsrcCodec(uint32_t ssrc, discord::dave::Codec codec);
    void addConnectedUser(const std::string &userId);
    void removeConnectedUser(const std::string &userId);
    void applyKeyRatchetForSsrc(uint32_t ssrc, uint64_t userId);

    Platform::Signal<int, std::string> sendKeyPackage;
    Platform::Signal<int, std::string> sendCommitWelcome;
    Platform::Signal<int> sendReadyForTransition;
    Platform::Signal<int> sendInvalidCommitWelcome;
    Platform::Signal<bool> daveStateChanged;

private:
    void reinit();
    void completeTransition();

    std::unique_ptr<discord::dave::mls::ISession> mlsSession;
    std::unique_ptr<discord::dave::IEncryptor> daveEncryptor;
    std::unordered_map<uint32_t, std::unique_ptr<discord::dave::IDecryptor>> daveDecryptors;

    uint16_t protocolVersion = 0;
    uint16_t pendingProtocolVersion = 0;
    Core::Snowflake channelId;
    Core::Snowflake userId;
    uint32_t localSsrc = 0;
    std::unordered_map<uint32_t, discord::dave::Codec> extraCodecs;

    std::set<std::string> connectedUserIds;
    int pendingTransitionId = -1;
    bool daveEnabled = false;
    bool daveDowngraded = false;
    bool pendingTransitionReady = false;

    const std::unordered_map<uint32_t, uint64_t> &ssrcMap;

    std::shared_ptr<::mlspp::SignaturePrivateKey> transientKey;
};

} // namespace Voice
} // namespace Discord
} // namespace Styx
