#pragma once

#include "Platform/Trackable.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "Platform/WorkerLoop.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <opus.h>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Audio/IAudioBackend.hpp"
#include "Discord/Events.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Styx {
namespace Core {
namespace Audio {

class AudioPipeline;

struct VoiceParticipant
{
    Snowflake userId;
    bool selfMute = false;
    bool selfDeaf = false;
    bool serverMute = false;
    bool serverDeaf = false;
    bool suppress = false;
    bool speaking = false;
    bool streaming = false; // Go Live
};

class VoiceManager : public Platform::Trackable
{
public:
    explicit VoiceManager(Snowflake accountId, const ProxyConfig &proxy);
    ~VoiceManager() override;

    void handleVoiceStateUpdate(const Discord::VoiceState &state);
    void handleVoiceServerUpdate(const Discord::VoiceServerUpdate &event);

    void disconnect();

    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] Discord::Voice::VoiceClient::State clientState() const;
    [[nodiscard]] Snowflake currentChannelId() const { return channelId; }
    [[nodiscard]] Snowflake currentGuildId() const { return guildId; }
    [[nodiscard]] const std::string &sessionId() const { return voiceSessionId; }

    // Mixes a watched Go Live stream's audio into the call output.
    void playStreamAudio(std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, const std::string &opus);

    void setInputGain(float gain);
    void setOutputVolume(float volume);
    [[nodiscard]] float inputGain() const { return cachedInputGain; }
    [[nodiscard]] float outputVolume() const { return cachedOutputVolume; }
    void setUserVolume(Snowflake userId, float volume);
    void setVadThreshold(float threshold);

    void setOpusApplication(int application);
    void setOpusBitrate(int bitrate);
    void setOpusComplexity(int complexity);
    void setOpusSignalType(int signalType);
    void setOpusFec(bool enabled);
    void setOpusPacketLossPercent(int percent);

    void setNoiseSuppressionEnabled(bool enabled);
    void setUseRnnoiseVad(bool enabled);

    [[nodiscard]] int opusApplication() const { return cachedOpusApplication; }
    [[nodiscard]] int opusBitrate() const { return cachedOpusBitrate; }
    [[nodiscard]] int opusComplexity() const { return cachedOpusComplexity; }
    [[nodiscard]] int opusSignalType() const { return cachedOpusSignalType; }
    [[nodiscard]] bool opusFec() const { return cachedOpusFec; }
    [[nodiscard]] int opusPacketLossPercent() const { return cachedOpusPacketLossPercent; }
    [[nodiscard]] bool noiseSuppression() const { return cachedNoiseSuppression; }
    [[nodiscard]] bool rnnoiseVad() const { return cachedUseRnnoiseVad; }

    [[nodiscard]] std::vector<AudioDeviceInfo> availableInputDevices() const;
    [[nodiscard]] std::vector<AudioDeviceInfo> availableOutputDevices() const;
    [[nodiscard]] std::string currentInputDevice() const { return currentInputDeviceId; }
    [[nodiscard]] std::string currentOutputDevice() const { return currentOutputDeviceId; }
    void setInputDevice(const std::string &deviceId);
    void setOutputDevice(const std::string &deviceId);

    [[nodiscard]] std::vector<VoiceParticipant> currentParticipants() const;
    [[nodiscard]] const VoiceParticipant *participant(Snowflake userId) const;
    [[nodiscard]] int channelVoiceUserCount(Snowflake channelId) const;
    [[nodiscard]] std::vector<Snowflake> channelVoiceUsers(Snowflake channelId) const;
    [[nodiscard]] std::optional<Discord::VoiceState> voiceStateForUser(Snowflake userId) const;

    void setUserMuted(Snowflake userId, bool muted);
    [[nodiscard]] bool isUserMuted(Snowflake userId) const;

    [[nodiscard]] bool isDaveEnabled() const;
    [[nodiscard]] const std::string &privacyCode() const { return cachedPrivacyCode; }
    void requestVerificationCode(Snowflake targetUserId, std::function<void(const std::string &)> callback);

public:
    Platform::Signal<> voiceConnected, voiceDisconnected, voiceStateChanged;
    Platform::Signal<float> audioLevelChanged;
    Platform::Signal<bool> speakingChanged;
    Platform::Signal<> devicesChanged;
    Platform::Signal<Snowflake> participantJoined, participantLeft, participantUpdated;
    Platform::Signal<Snowflake, bool> participantSpeakingChanged;
    Platform::Signal<Snowflake, Snowflake, bool> channelVoiceMemberChanged;
    Platform::Signal<Snowflake, Snowflake> participantVoiceStateChanged;
    Platform::Signal<> participantsCleared;
    Platform::Signal<Snowflake, float> userAudioLevelChanged;
    Platform::Signal<std::string> privacyCodeChanged;

private:
    void onVoiceClientConnected();
    void onVoiceClientDisconnected();
    void onVoiceClientStateChanged(Discord::Voice::VoiceClient::State state);
    void onDevicesChanged(const std::vector<AudioDeviceInfo> &inputs, const std::vector<AudioDeviceInfo> &outputs);

private:
    void connectToVoiceServer(const std::string &endpoint, const std::string &token);
    void stopVoiceThread();
    void populateParticipantsFromCache();
    IAudioBackend *backend() const;

private:
    Snowflake accountId;
    ProxyConfig proxy;

    std::string voiceSessionId;
    Snowflake guildId;
    Snowflake channelId;
    bool selfMute = false;
    bool selfDeaf = false;

    // pending data: VOICE_SERVER_UPDATE may arrive before VOICE_STATE_UPDATE
    // both are needed to connect
    struct PendingConnection
    {
        std::string token;
        std::string endpoint;
        Snowflake guildId;
        bool hasServerUpdate = false;
        bool hasStateUpdate = false;
    };
    PendingConnection pending;

    std::string currentInputDeviceId;
    std::string currentOutputDeviceId;

    int cachedOpusApplication = OPUS_APPLICATION_VOIP;
    int cachedOpusBitrate = 64000;
    int cachedOpusComplexity = 5;
    int cachedOpusSignalType = OPUS_SIGNAL_VOICE;
    bool cachedOpusFec = true;
    int cachedOpusPacketLossPercent = 0;
    bool cachedNoiseSuppression = true;
#ifdef STYX_HAVE_RNNOISE
    bool cachedUseRnnoiseVad = true;
#else
    bool cachedUseRnnoiseVad = false;
#endif
    std::vector<AudioDeviceInfo> cachedInputDevices;
    std::vector<AudioDeviceInfo> cachedOutputDevices;
    float cachedInputGain = 1.0f;
    float cachedOutputVolume = 1.0f;

    std::unordered_map<Snowflake, VoiceParticipant> participants;
    std::unordered_set<Snowflake> mutedUsers;
    std::unordered_map<Snowflake, Discord::VoiceState> knownVoiceStates;
    std::string cachedPrivacyCode;

    std::unique_ptr<Platform::WorkerLoop> voiceLoop;
    std::shared_ptr<std::atomic<bool>> m_voiceAlive = std::make_shared<std::atomic<bool>>(true);
    Discord::Voice::VoiceClient *voiceClient = nullptr;
    AudioPipeline *audioPipeline = nullptr;
    unsigned int voiceGeneration = 0;

    mutable std::unique_ptr<IAudioBackend> audioBackend;
};

} // namespace Audio
} // namespace Core
} // namespace Styx
