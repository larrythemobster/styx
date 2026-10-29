#pragma once

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Platform/Trackable.hpp"
#include "VoiceEnums.hpp"
#include "VoiceEntities.hpp"

namespace Styx::Platform { class WorkerLoop; }

namespace Styx::Discord::Voice {

class VoiceGateway : public Platform::Trackable
{
public:
    VoiceGateway(const std::string &endpoint, Core::Snowflake serverId, Core::Snowflake channelId,
                 Core::Snowflake userId, const std::string &sessionId, const std::string &token,
                 const Core::ProxyConfig &proxy, Platform::WorkerLoop *ownerLoop = nullptr);
    ~VoiceGateway() override;

    void start();
    void stop();
    void hardStop();

    // A Go Live session identifies with video and its "screen" layer. Set before start().
    void setVideoEnabled(bool enabled) { videoEnabled = enabled; }

    void sendSelectProtocol(const std::string &address, int port, const std::string &mode);
    void sendSelectProtocolWebRtc(const std::string &sdp, const std::vector<Codec> &codecs,
                                  const std::string &rtcConnectionId);
    void sendVideo(const Core::OrderedJson &d);
    void sendMediaSinkWants(const Core::OrderedJson &d);
    void sendSpeaking(int flags, int delay, uint32_t ssrc);
    void sendBinaryPayload(int opcode, const std::string &data);
    void sendDaveReadyForTransition(int transitionId);
    void sendDaveInvalidCommitWelcome(int transitionId);

    [[nodiscard]] uint64_t currentGeneration() const { return generation.load(); }

    Platform::Signal<> connected;
    Platform::Signal<VoiceCloseCode, const std::string &> disconnected;
    Platform::Signal<int, int> reconnecting;
    Platform::Signal<int> helloReceived;
    Platform::Signal<const VoiceReady &> readyReceived;
    Platform::Signal<const SessionDescription &> sessionDescriptionReceived;
    Platform::Signal<const SpeakingData &> speakingReceived;
    Platform::Signal<const VideoData &> videoReceived;
    Platform::Signal<const ClientConnectData &> clientConnected;
    Platform::Signal<const std::vector<std::string> &> clientsConnected;
    Platform::Signal<Core::Snowflake> clientDisconnected;
    Platform::Signal<> resumed;
    Platform::Signal<const nlohmann::json &> payloadReceived;
    Platform::Signal<int, const std::string &> binaryPayloadReceived;
    Platform::Signal<int, int> daveTransitionPrepare;
    Platform::Signal<int> daveTransitionExecute;
    Platform::Signal<int, int> daveEpochPrepare;

private:
    void dispatch(std::function<void()> task);
    void sendPayload(VoiceOpCode op, const Core::OrderedJson &d);
    void sendPayload(const std::string &data);
    void onPayloadReceived(const nlohmann::json &root);
    void handleHello(const nlohmann::json &data);
    void handleReady(const nlohmann::json &data);
    void handleSessionDescription(const nlohmann::json &data);
    void handleSpeaking(const nlohmann::json &data);
    void handleHeartbeatAck(uint64_t nonce);
    void handleResumed();
    void handleClientConnect(const nlohmann::json &data);
    void handleClientsConnected(const nlohmann::json &data);
    void handleClientDisconnect(const nlohmann::json &data);
    void handleDavePrepareTransition(const nlohmann::json &data);
    void handleDaveExecuteTransition(const nlohmann::json &data);
    void handleDavePrepareEpoch(const nlohmann::json &data);
    void identify();
    void resume();
    bool isFatalCloseCode(VoiceCloseCode code) const;
    void networkLoop();
    void heartbeatLoop();

    std::string endpoint;
    Core::Snowflake serverId;
    Core::Snowflake channelId;
    Core::Snowflake userId;
    std::string sessionId;
    std::string token;
    Core::ProxyConfig proxy;
    Platform::WorkerLoop *ownerLoop;
    bool videoEnabled = false;
    std::atomic<bool> running{ false };
    std::mutex curlMutex;
    CURL *curl = nullptr;
    std::atomic<bool> wantToClose{ false };
    std::thread networkThread;
    std::chrono::steady_clock::time_point closeTime;
    static constexpr std::chrono::milliseconds closeTimeout{ 1000 };
    std::atomic<int> heartbeatInterval{ 0 };
    std::mutex heartbeatMutex;
    std::condition_variable heartbeatCv;
    std::thread heartbeatThread;
    std::atomic<bool> heartbeatAckReceived{ true };
    std::atomic<uint64_t> lastSentNonce{ 0 };
    std::atomic<int> lastReceivedSeq{ -1 };
    std::atomic<bool> shouldReconnect{ false };
    std::atomic<bool> canResume{ false };
    std::atomic<bool> isResuming{ false };
    std::atomic<int> reconnectAttempts{ 0 };
    static constexpr int maxReconnectAttempts = 5;
    std::atomic<uint64_t> generation{ 0 };
};

} // namespace Styx::Discord::Voice
