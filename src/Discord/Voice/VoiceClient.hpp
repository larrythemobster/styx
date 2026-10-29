#pragma once

#include "Platform/Trackable.hpp"
#include "Platform/WorkerLoop.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "VoiceEnums.hpp"
#include "VoiceEntities.hpp"

namespace Styx::Discord::Voice {

class VoiceGateway;
class UdpTransport;
class VoiceEncryption;
class DaveSession;

class VoiceClient : public Platform::Trackable
{
public:
    enum class State {
        Disconnected, Connecting, Identifying, WaitingForReady, DiscoveringIP,
        SelectingProtocol, WaitingForSession, Connected,
    };

    VoiceClient(const std::string &endpoint, const std::string &token, Core::Snowflake serverId,
                Core::Snowflake channelId, Core::Snowflake userId, const std::string &sessionId,
                const Core::ProxyConfig &proxy, Platform::WorkerLoop *loop = nullptr);
    ~VoiceClient() override;

    void start();
    void stop();
    [[nodiscard]] State state() const { return currentState; }
    [[nodiscard]] uint32_t ssrc() const { return localSsrc; }
    [[nodiscard]] const std::string &encryptionMode() const { return selectedMode; }
    [[nodiscard]] const std::string &secretKey() const { return sessionKey; }
    void seedConnectedUsers(const std::vector<Core::Snowflake> &userIds);
    void sendAudio(const std::string &opusData);
    void setSpeaking(bool speaking);
    bool isDaveEnabled() const;
    void requestVerificationCode(Core::Snowflake targetUserId, std::function<void(const std::string &)> callback);

    Platform::Signal<State> stateChanged;
    Platform::Signal<> connected;
    Platform::Signal<> disconnected;
    Platform::Signal<SpeakingData> speakingReceived;
    Platform::Signal<ClientConnectData> clientConnected;
    Platform::Signal<Core::Snowflake> clientDisconnected;
    Platform::Signal<uint32_t, uint16_t, uint32_t, std::string> audioReceived;
    Platform::Signal<std::string> privacyCodeChanged;

private:
    void onGatewayConnected();
    void onGatewayDisconnected(VoiceCloseCode code, const std::string &reason);
    void onGatewayReady(const VoiceReady &data);
    void onSessionDescription(const SessionDescription &desc);
    void onSpeaking(const SpeakingData &data);
    void onClientsConnect(const std::vector<std::string> &userIds);
    void onClientConnect(const ClientConnectData &data);
    void onClientDisconnect(Core::Snowflake userId);
    void onGatewayResumed();
    void onIpDiscovered(const std::string &ip, int port);
    void onIpDiscoveryFailed(const std::string &error);
    void onDatagram(const std::string &data);
    void setState(State state);
    void sendSilence();
    void cleanupTransport();
    void ensureDaveSession(uint16_t protocolVersion);

    std::unique_ptr<VoiceGateway> gateway;
    std::unique_ptr<UdpTransport> udpTransport;
    std::string endpoint;
    std::string token;
    Core::ProxyConfig proxy;
    Core::Snowflake serverId;
    Core::Snowflake channelId;
    Core::Snowflake userId;
    std::string sessionId;
    std::atomic<State> currentState{ State::Disconnected };
    uint32_t localSsrc = 0;
    std::string serverIp;
    int serverPort = 0;
    std::vector<std::string> serverModes;
    std::string selectedMode;
    std::string sessionKey;
    std::unique_ptr<VoiceEncryption> encryption;
    Platform::WorkerLoop *m_loop = nullptr;
    Platform::TimerId m_keepaliveTimerId{0};
    uint16_t rtpSequence = 0;
    uint32_t rtpTimestamp = 0;
    std::chrono::steady_clock::time_point rtpEpoch;
    std::chrono::steady_clock::time_point lastAudioSendTime;
    bool newTalkspurt = false;
    std::unique_ptr<DaveSession> daveSession;
    std::atomic<bool> daveEnabled{false};
    std::set<std::string> connectedUserIds;
    std::unordered_map<uint32_t, uint64_t> ssrcToUserIdMap;
    static constexpr int KEEPALIVE_INTERVAL_MS = 10000;
};

} // namespace Styx::Discord::Voice
