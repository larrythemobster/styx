#include "VoiceManager.hpp"
#include "AudioPipeline.hpp"
#include "Platform/Dispatcher.hpp"
#include <future>
#include <atomic>
#include <algorithm>
#include <string_view>

#include "Core/Logging.hpp"
#include "Core/SettingsKeys.hpp"

namespace Styx {
namespace Core {
namespace Audio {

namespace {
std::string decodeDeviceId(std::string_view hex)
{
    std::string result;
    int low = -1;
    for (auto it = hex.rbegin(); it != hex.rend(); ++it) {
        const char c = *it;
        const int value = c >= '0' && c <= '9' ? c - '0' :
                          c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                          c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (value < 0) continue;
        if (low < 0) low = value;
        else { result.push_back(static_cast<char>((value << 4) | low)); low = -1; }
    }
    if (low >= 0) result.push_back(static_cast<char>(low));
    std::reverse(result.begin(), result.end());
    return result;
}
}

VoiceManager::VoiceManager(Snowflake accountId, const ProxyConfig &proxy)
    : accountId(accountId), proxy(proxy)
{
    const int inVol = Settings::getInt(Settings::Keys::VoiceInputVolume, Settings::Defaults::VoiceInputVolume);
    cachedInputGain = static_cast<float>(inVol) / 100.0f;
    const int outVol = Settings::getInt(Settings::Keys::VoiceOutputVolume, Settings::Defaults::VoiceOutputVolume);
    cachedOutputVolume = static_cast<float>(outVol) / 100.0f;

    const std::string inDev = Settings::getString(Settings::Keys::VoiceInputDevice, Settings::Defaults::VoiceInputDevice);
    if (!inDev.empty())
        currentInputDeviceId = decodeDeviceId(inDev);
    const std::string outDev = Settings::getString(Settings::Keys::VoiceOutputDevice, Settings::Defaults::VoiceOutputDevice);
    if (!outDev.empty())
        currentOutputDeviceId = decodeDeviceId(outDev);
}

VoiceManager::~VoiceManager()
{
    stopVoiceThread();
}

IAudioBackend *VoiceManager::backend() const
{
    if (!audioBackend) {
        audioBackend = IAudioBackend::create();
        audioBackend->setInputGain(cachedInputGain);
        audioBackend->setOutputVolume(cachedOutputVolume);
        if (!currentInputDeviceId.empty())
            audioBackend->setInputDevice(currentInputDeviceId);
        if (!currentOutputDeviceId.empty())
            audioBackend->setOutputDevice(currentOutputDeviceId);
        Platform::Guard<VoiceManager> guard(const_cast<VoiceManager *>(this));
        const_cast<VoiceManager *>(this)->track(audioBackend->devicesChanged.connect(
            [guard](const auto &inputs, const auto &outputs) {
                Platform::Dispatcher::main().post([guard, inputs, outputs]() {
                    if (guard) guard->onDevicesChanged(inputs, outputs);
                });
            }));
    }
    return audioBackend.get();
}

void VoiceManager::handleVoiceStateUpdate(const Discord::VoiceState &state)
{
    Snowflake userId = state.userId.get();
    bool isMe = (userId == accountId);

    // cache all non-self voice states so we can populate participants when joining a channel
    if (!isMe) {
        Snowflake oldChannel;
        bool hadOld = false;
        Discord::VoiceState oldState;
        auto oldIt = knownVoiceStates.find(userId);
        if (oldIt != knownVoiceStates.end()) {
            hadOld = true;
            oldState = oldIt->second;
            if (!oldIt->second.channelId.isNull())
                oldChannel = oldIt->second.channelId.get();
        }

        bool leftVoice = state.channelId.isNull() || !state.channelId.get().isValid();
        Snowflake newChannel = leftVoice ? Snowflake::Invalid : state.channelId.get();

        if (leftVoice)
            knownVoiceStates.erase(userId);
        else
            knownVoiceStates[userId] = state;

        if (oldChannel != newChannel) {
            if (oldChannel.isValid())
                channelVoiceMemberChanged.publish(oldChannel, userId, false);
            if (newChannel.isValid())
                channelVoiceMemberChanged.publish(newChannel, userId, true);
        } else if (newChannel.isValid() && hadOld) {
            bool stateDiffers = oldState.selfMute.get() != state.selfMute.get() ||
                                oldState.selfDeaf.get() != state.selfDeaf.get() ||
                                oldState.mute.get() != state.mute.get() ||
                                oldState.deaf.get() != state.deaf.get() ||
                                oldState.suppress.get() != state.suppress.get() ||
                                oldState.selfStream.valueOr(false) != state.selfStream.valueOr(false);
            if (stateDiffers)
                participantVoiceStateChanged.publish(newChannel, userId);
        }
    }

    if (isMe) {
        if (state.channelId.isNull() || !state.channelId.get().isValid()) {
            log(LogLevel::Info, LogVoice) << "Voice state: disconnected from channel";
            Snowflake oldChannel = channelId;
            voiceSessionId.clear();
            channelId = Snowflake::Invalid;
            guildId = Snowflake::Invalid;
            pending = {};

            if (oldChannel.isValid())
                channelVoiceMemberChanged.publish(oldChannel, accountId, false);

            if (voiceLoop)
                disconnect();
            return;
        }

        Snowflake newChannelId = state.channelId.get();
        Snowflake oldSelfChannel = channelId;
        bool channelChanged = (channelId != newChannelId);

        voiceSessionId = state.sessionId.get();
        channelId = newChannelId;
        guildId = state.guildId.hasValue() ? state.guildId.get() : Snowflake::Invalid;

        if (channelChanged) {
            if (!participants.empty()) {
                participants.clear();
                participantsCleared.publish();
            }
            populateParticipantsFromCache();

            if (oldSelfChannel.isValid())
                channelVoiceMemberChanged.publish(oldSelfChannel, accountId, false);
            channelVoiceMemberChanged.publish(newChannelId, accountId, true);
        }

        bool wasMuted = selfMute;
        bool wasDeaf = selfDeaf;
        selfMute = state.selfMute.get();
        selfDeaf = state.selfDeaf.get();

        log(LogLevel::Info, LogVoice) << "Voice state: session =" << voiceSessionId
                         << "channel =" << channelId << "guild =" << guildId;

        if (!channelChanged && channelId.isValid() && (selfMute != wasMuted || selfDeaf != wasDeaf))
            participantVoiceStateChanged.publish(channelId, accountId);

        if (audioPipeline && voiceLoop && (selfMute != wasMuted || selfDeaf != wasDeaf)) {
            if (selfMute || selfDeaf)
                voiceLoop->post([p = audioPipeline]() { p->stopCapture(); });
            else
                voiceLoop->post([p = audioPipeline]() { p->startCapture(); });

            if (selfDeaf != wasDeaf)
                voiceLoop->post([p = audioPipeline, deaf = selfDeaf]() { p->setDeafened(deaf); });
        }

        pending.hasStateUpdate = true;

        if (pending.hasServerUpdate && pending.guildId == guildId) {
            connectToVoiceServer(pending.endpoint, pending.token);
            pending = {};
        }
    }

    // others
    if (!channelId.isValid() || isMe)
        return;

    bool userLeftChannel = state.channelId.isNull() || !state.channelId.get().isValid() || state.channelId.get() != channelId;

    if (userLeftChannel) {
        if (participants.erase(userId)) {
            if (audioPipeline && voiceLoop)
                voiceLoop->post([p = audioPipeline, userId]() { p->removeUser(userId); });
            participantLeft.publish(userId);
            someoneLeft.publish(userId);
        }
    } else {
        auto it = participants.find(userId);
        bool isNew = (it == participants.end());

        VoiceParticipant &p = isNew ? participants[userId] : it->second;
        if (isNew)
            p.userId = userId;

        p.selfMute = state.selfMute.get();
        p.selfDeaf = state.selfDeaf.get();
        p.serverMute = state.mute.get();
        p.serverDeaf = state.deaf.get();
        p.suppress = state.suppress.get();
        p.streaming = state.selfStream.valueOr(false);

        if (isNew) {
            participantJoined.publish(userId);
            someoneJoined.publish(userId);
        } else
            participantUpdated.publish(userId);
    }
}

void VoiceManager::handleVoiceServerUpdate(const Discord::VoiceServerUpdate &event)
{
    Snowflake eventGuildId = event.guildId.get();

    if (event.endpoint.isNull() || event.endpoint.get().empty()) {
        log(LogLevel::Info, LogVoice) << "Voice server update with null endpoint, waiting for new one";
        return;
    }

    if (!proxy.canRelayUdp()) {
        log(LogLevel::Warning, LogVoice) << "Refusing voice connection: an HTTP proxy cannot relay voice UDP";
        return;
    }

    std::string eventEndpoint = event.endpoint.get();
    std::string eventToken = event.token.get();

    log(LogLevel::Info, LogVoice) << "Voice server update: guild =" << eventGuildId
                     << "endpoint =" << eventEndpoint;

    if (voiceLoop && guildId == eventGuildId) {
        stopVoiceThread();
        connectToVoiceServer(eventEndpoint, eventToken);
        return;
    }

    pending.token = eventToken;
    pending.endpoint = eventEndpoint;
    pending.guildId = eventGuildId;
    pending.hasServerUpdate = true;

    if (pending.hasStateUpdate && !voiceSessionId.empty()) {
        connectToVoiceServer(eventEndpoint, eventToken);
        pending = {};
    }
}

void VoiceManager::disconnect()
{
    stopVoiceThread();
    pending = {};
    voiceDisconnected.publish();
    voiceStateChanged.publish();
}

bool VoiceManager::isConnected() const
{
    return voiceClient &&
           voiceClient->state() == Discord::Voice::VoiceClient::State::Connected;
}

Discord::Voice::VoiceClient::State VoiceManager::clientState() const
{
    if (!voiceClient)
        return Discord::Voice::VoiceClient::State::Disconnected;
    return voiceClient->state();
}

void VoiceManager::playStreamAudio(std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, const std::string &opus)
{
    if (audioPipeline && voiceLoop && !selfDeaf)
        voiceLoop->post([p = audioPipeline, ssrc, sequence, timestamp, opus]() {
            p->onAudioReceived(ssrc, sequence, timestamp, opus);
        });
}

void VoiceManager::setInputGain(float gain)
{
    cachedInputGain = gain;
    if (audioBackend)
        audioBackend->setInputGain(gain);
    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, gain]() { p->setInputGain(gain); });
}

void VoiceManager::setOutputVolume(float volume)
{
    cachedOutputVolume = volume;
    if (audioBackend)
        audioBackend->setOutputVolume(volume);
    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, volume]() { p->setOutputVolume(volume); });
}

void VoiceManager::setUserVolume(Snowflake userId, float volume)
{
    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, userId, volume]() { p->setUserVolume(userId, volume); });
}

void VoiceManager::setVadThreshold(float threshold)
{
    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, threshold]() { p->setVadThreshold(threshold); });
}
void VoiceManager::setOpusApplication(int application)
{
    cachedOpusApplication = application;

    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, application]() { p->setOpusApplication(application); });
}

void VoiceManager::setOpusBitrate(int bitrate)
{
    cachedOpusBitrate = bitrate;

    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, bitrate]() { p->setOpusBitrate(bitrate); });
}
void VoiceManager::setOpusComplexity(int complexity)
{
    cachedOpusComplexity = complexity;

    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, complexity]() { p->setOpusComplexity(complexity); });
}

void VoiceManager::setOpusSignalType(int signalType)
{
    cachedOpusSignalType = signalType;

    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, signalType]() { p->setOpusSignalType(signalType); });
}
void VoiceManager::setOpusFec(bool enabled)
{
    cachedOpusFec = enabled;

    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, enabled]() { p->setOpusFec(enabled); });
}

void VoiceManager::setOpusPacketLossPercent(int percent)
{
    cachedOpusPacketLossPercent = percent;

    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, percent]() { p->setOpusPacketLossPercent(percent); });
}
void VoiceManager::setNoiseSuppressionEnabled(bool enabled)
{
    cachedNoiseSuppression = enabled;

    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, enabled]() { p->setNoiseSuppressionEnabled(enabled); });
}

void VoiceManager::setUseRnnoiseVad(bool enabled)
{
    cachedUseRnnoiseVad = enabled;

    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, enabled]() { p->setUseRnnoiseVad(enabled); });
}
std::vector<AudioDeviceInfo> VoiceManager::availableInputDevices() const
{
    if (voiceLoop)
        return cachedInputDevices;
    return backend()->availableInputDevices();
}

std::vector<AudioDeviceInfo> VoiceManager::availableOutputDevices() const
{
    if (voiceLoop)
        return cachedOutputDevices;
    return backend()->availableOutputDevices();
}

void VoiceManager::setInputDevice(const std::string &deviceId)
{
    currentInputDeviceId = deviceId;
    if (audioBackend)
        audioBackend->setInputDevice(deviceId);
    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, deviceId]() { p->setInputDevice(deviceId); });
}

void VoiceManager::setOutputDevice(const std::string &deviceId)
{
    currentOutputDeviceId = deviceId;
    if (audioBackend)
        audioBackend->setOutputDevice(deviceId);
    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, deviceId]() { p->setOutputDevice(deviceId); });
}
std::vector<VoiceParticipant> VoiceManager::currentParticipants() const
{
    std::vector<VoiceParticipant> result;
    result.reserve(participants.size());
    for (const auto &[id, participant] : participants)
        result.push_back(participant);
    return result;
}

const VoiceParticipant *VoiceManager::participant(Snowflake userId) const
{
    auto it = participants.find(userId);
    if (it == participants.end())
        return nullptr;
    return &it->second;
}

void VoiceManager::setUserMuted(Snowflake userId, bool muted)
{
    if (muted)
        mutedUsers.insert(userId);
    else
        mutedUsers.erase(userId);

    setUserVolume(userId, muted ? 0.0f : 1.0f);
}

bool VoiceManager::isUserMuted(Snowflake userId) const
{
    return mutedUsers.contains(userId);
}

int VoiceManager::channelVoiceUserCount(Snowflake targetChannelId) const
{
    if (!targetChannelId.isValid())
        return 0;

    int count = 0;
    for (auto it = knownVoiceStates.begin(); it != knownVoiceStates.end(); ++it) {
        if (!it->second.channelId.isNull() && it->second.channelId.get() == targetChannelId)
            ++count;
    }

    // include self in count
    if (channelId == targetChannelId && channelId.isValid())
        ++count;

    return count;
}

std::vector<Snowflake> VoiceManager::channelVoiceUsers(Snowflake targetChannelId) const
{
    std::vector<Snowflake> users;
    if (!targetChannelId.isValid())
        return users;

    if (channelId == targetChannelId && channelId.isValid())
        users.push_back(accountId);

    for (auto it = knownVoiceStates.begin(); it != knownVoiceStates.end(); ++it) {
        if (!it->second.channelId.isNull() && it->second.channelId.get() == targetChannelId)
            users.push_back(it->first);
    }

    return users;
}

std::optional<Discord::VoiceState> VoiceManager::voiceStateForUser(Snowflake userId) const
{
    if (userId == accountId && channelId.isValid()) {
        Discord::VoiceState state;
        state.userId = accountId;
        state.channelId = channelId;
        if (guildId.isValid())
            state.guildId = guildId;
        state.selfMute = selfMute;
        state.selfDeaf = selfDeaf;
        state.sessionId = voiceSessionId;
        return state;
    }

    auto it = knownVoiceStates.find(userId);
    if (it == knownVoiceStates.end())
        return std::nullopt;
    return it->second;
}

bool VoiceManager::isDaveEnabled() const
{
    return voiceClient && voiceClient->isDaveEnabled();
}

void VoiceManager::requestVerificationCode(Snowflake targetUserId, std::function<void(const std::string &)> callback)
{
    if (!voiceClient) {
        callback(std::string());
        return;
    }
    if (voiceLoop) {
        voiceLoop->post([vc = voiceClient, targetUserId, cb = std::move(callback)]() mutable {
            vc->requestVerificationCode(targetUserId, std::move(cb));
        });
    }
}

void VoiceManager::connectToVoiceServer(const std::string &endpoint, const std::string &token)
{
    if (voiceSessionId.empty()) {
        log(LogLevel::Warning, LogVoice) << "Cannot connect to voice: no session ID";
        return;
    }

    stopVoiceThread();

    log(LogLevel::Info, LogVoice) << "Creating voice loop for endpoint" << endpoint
                     << "guild" << guildId << "channel" << channelId;

    Styx::Platform::WorkerOptions opts;
    opts.name = "VoiceThread";
    opts.initComMta = true;
    voiceLoop = std::make_unique<Styx::Platform::WorkerLoop>(opts);

    Snowflake serverId = guildId.isValid() ? guildId : channelId;
    voiceClient = new Discord::Voice::VoiceClient(endpoint, token, serverId, channelId, accountId, voiceSessionId, proxy, voiceLoop.get());
    audioPipeline = new AudioPipeline(*voiceLoop);

    std::vector<Snowflake> channelUsers;
    for (auto it = knownVoiceStates.begin(); it != knownVoiceStates.end(); ++it) {
        const auto &vs = it->second;
        if (!vs.channelId.isNull() && vs.channelId.get() == channelId)
            channelUsers.push_back(it->first);
    }
    voiceClient->seedConnectedUsers(channelUsers);

    cachedInputDevices = backend()->availableInputDevices();
    cachedOutputDevices = backend()->availableOutputDevices();

    audioPipeline->track(voiceClient->audioReceived.connect(
        [ap = audioPipeline](std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, const std::string &opus) {
            ap->onAudioReceived(ssrc, sequence, timestamp, opus);
        }));
    voiceClient->track(audioPipeline->encodedAudioReady.connect(
        [vc = voiceClient](const std::string &opus) { vc->sendAudio(opus); }));
    voiceClient->track(audioPipeline->speakingChanged.connect(
        [vc = voiceClient](bool speaking) { vc->setSpeaking(speaking); }));
    audioPipeline->track(voiceClient->speakingReceived.connect(
        [ap = audioPipeline](const Discord::Voice::SpeakingData &data) {
            if (data.userId.hasValue() && data.userId->isValid() && data.ssrc.get() != 0)
                ap->setSsrcUserId(data.ssrc, data.userId.get());
        }));
    audioPipeline->track(voiceClient->clientConnected.connect(
        [ap = audioPipeline](const Discord::Voice::ClientConnectData &data) {
            if (data.userId.hasValue() && data.userId->isValid() && data.audioSsrc.get() != 0)
                ap->setSsrcUserId(data.audioSsrc, data.userId.get());
        }));

    const unsigned int gen = voiceGeneration;
    auto alive = m_voiceAlive;
    track(voiceClient->clientConnected.connect([this, gen, alive](const Discord::Voice::ClientConnectData &data) {
        Platform::Dispatcher::main().post([this, gen, alive, data]() {
            if (!*alive || gen != voiceGeneration || !data.userId.hasValue())
                return;
            Snowflake userId = data.userId.get();
            if (!userId.isValid() || userId == accountId || participants.contains(userId))
                return;
            VoiceParticipant p;
            p.userId = userId;
            participants.emplace(userId, p);
            participantJoined.publish(userId);
            someoneJoined.publish(userId);
        });
    }));
    track(voiceClient->clientDisconnected.connect([this, gen, alive](Snowflake userId) {
        Platform::Dispatcher::main().post([this, gen, alive, userId]() {
            if (!*alive || gen != voiceGeneration)
                return;
            if (participants.erase(userId)) {
                if (audioPipeline && voiceLoop)
                    voiceLoop->post([p = audioPipeline, userId]() { p->removeUser(userId); });
                participantLeft.publish(userId);
                someoneLeft.publish(userId);
            }
        });
    }));
    track(voiceClient->speakingReceived.connect([this, gen, alive](const Discord::Voice::SpeakingData &data) {
        Platform::Dispatcher::main().post([this, gen, alive, data]() {
            if (!*alive || gen != voiceGeneration || !data.userId.hasValue() || !data.userId->isValid())
                return;
            Snowflake userId = data.userId.get();
            auto it = participants.find(userId);
            if (it == participants.end())
                return;
            bool wasSpeaking = it->second.speaking;
            it->second.speaking = data.speaking.get() != 0;
            if (it->second.speaking != wasSpeaking)
                participantSpeakingChanged.publish(userId, it->second.speaking);
        });
    }));
    track(audioPipeline->userAudioLevelChanged.connect([this, alive](Snowflake userId, float level) {
        Platform::Dispatcher::main().post([this, alive, userId, level]() {
            if (*alive) userAudioLevelChanged.publish(userId, level);
        });
    }));
    track(voiceClient->connected.connect([this, gen, alive]() {
        Platform::Dispatcher::main().post([this, gen, alive]() {
            if (*alive && gen == voiceGeneration) onVoiceClientConnected();
        });
    }));
    track(voiceClient->disconnected.connect([this, gen, alive]() {
        Platform::Dispatcher::main().post([this, gen, alive]() {
            if (*alive && gen == voiceGeneration) onVoiceClientDisconnected();
        });
    }));
    track(voiceClient->stateChanged.connect([this, gen, alive](Discord::Voice::VoiceClient::State state) {
        Platform::Dispatcher::main().post([this, gen, alive, state]() {
            if (*alive && gen == voiceGeneration) onVoiceClientStateChanged(state);
        });
    }));
    track(audioPipeline->audioLevelChanged.connect([this, alive](float level) {
        Platform::Dispatcher::main().post([this, alive, level]() {
            if (*alive) audioLevelChanged.publish(level);
        });
    }));
    track(audioPipeline->speakingChanged.connect([this, alive](bool speaking) {
        Platform::Dispatcher::main().post([this, alive, speaking]() {
            if (*alive) speakingChanged.publish(speaking);
        });
    }));
    track(voiceClient->privacyCodeChanged.connect([this, gen, alive](const std::string &code) {
        Platform::Dispatcher::main().post([this, gen, alive, code]() {
            if (!*alive || gen != voiceGeneration)
                return;
            cachedPrivacyCode = code;
            privacyCodeChanged.publish(code);
        });
    }));

    voiceLoop->post([vc = voiceClient]() {
        vc->start();
    });
}

void VoiceManager::stopVoiceThread()
{
    if (!voiceLoop)
        return;

    voiceGeneration++;
    if (m_voiceAlive)
        *m_voiceAlive = false;
    m_voiceAlive = std::make_shared<std::atomic<bool>>(true);

    bool hadParticipants = !participants.empty();
    participants.clear();
    mutedUsers.clear();
    cachedPrivacyCode.clear();
    if (hadParticipants)
        participantsCleared.publish();

    AudioPipeline *ap = audioPipeline;
    Discord::Voice::VoiceClient *vc = voiceClient;

    if (voiceLoop->isRunning()) {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        voiceLoop->post([ap, vc, promise]() {
            if (ap) ap->stop();
            if (vc) vc->stop();
            promise->set_value();
        });
        if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            log(LogLevel::Warning, LogVoice) << "Voice worker stop timed out";
        }
    }

    voiceLoop->stopAndWait();
    

    delete audioPipeline;
    audioPipeline = nullptr;

    delete voiceClient;
    voiceClient = nullptr;
    voiceLoop.reset();

    log(LogLevel::Debug, LogVoice) << "Voice loop stopped";
}

void VoiceManager::populateParticipantsFromCache()
{
    for (auto it = knownVoiceStates.begin(); it != knownVoiceStates.end(); ++it) {
        const auto &vs = it->second;
        if (vs.channelId.isNull() || vs.channelId.get() != channelId)
            continue;
        if (participants.contains(it->first))
            continue;

        VoiceParticipant p;
        p.userId = it->first;
        p.selfMute = vs.selfMute.get();
        p.selfDeaf = vs.selfDeaf.get();
        p.serverMute = vs.mute.get();
        p.serverDeaf = vs.deaf.get();
        p.suppress = vs.suppress.get();
        p.streaming = vs.selfStream.valueOr(false);
        participants.emplace(it->first, p);
        participantJoined.publish(it->first);
    }
}

void VoiceManager::onVoiceClientConnected()
{
    if (!audioPipeline)
        return;

    log(LogLevel::Info, LogVoice) << "Voice connection established for channel" << channelId;

    bool capturing = !selfMute && !selfDeaf;
    std::string inputId = currentInputDeviceId;
    std::string outputId = currentOutputDeviceId;
    IAudioBackend *audio = backend();
    int application = cachedOpusApplication;
    int bitrate = cachedOpusBitrate;
    int complexity = cachedOpusComplexity;
    int signalType = cachedOpusSignalType;
    bool fec = cachedOpusFec;
    int plp = cachedOpusPacketLossPercent;
    bool ns = cachedNoiseSuppression;
    bool nsVad = cachedUseRnnoiseVad;
    float inGain = cachedInputGain;
    float outVol = cachedOutputVolume;
    if (audioPipeline && voiceLoop) {
        voiceLoop->post([p = audioPipeline, audio, capturing, inputId, outputId,
                         application, bitrate, complexity, signalType, fec, plp, ns, nsVad,
                         inGain, outVol]() {
            p->setOpusApplication(application);
            p->setOpusBitrate(bitrate);
            p->setOpusComplexity(complexity);
            p->setOpusSignalType(signalType);
            p->setOpusFec(fec);
            p->setOpusPacketLossPercent(plp);
            p->setNoiseSuppressionEnabled(ns);
            p->setUseRnnoiseVad(nsVad);
            p->start(audio, capturing);
            p->setInputGain(inGain);
            p->setOutputVolume(outVol);
            if (!inputId.empty())
                p->setInputDevice(inputId);
            if (!outputId.empty())
                p->setOutputDevice(outputId);
        });
    }

    populateParticipantsFromCache();

    voiceConnected.publish();
    voiceStateChanged.publish();
}

void VoiceManager::onVoiceClientDisconnected()
{
    log(LogLevel::Info, LogVoice) << "Voice client disconnected";

    stopVoiceThread();

    voiceDisconnected.publish();
    voiceStateChanged.publish();
}

void VoiceManager::onVoiceClientStateChanged(Discord::Voice::VoiceClient::State state)
{
    log(LogLevel::Debug, LogVoice) << "VoiceManager: client state ->" << static_cast<int>(state);
    voiceStateChanged.publish();
}

void VoiceManager::onDevicesChanged(const std::vector<AudioDeviceInfo> &inputs, const std::vector<AudioDeviceInfo> &outputs)
{
    cachedInputDevices = inputs;
    cachedOutputDevices = outputs;
    devicesChanged.publish();
}

} // namespace Audio
} // namespace Core
} // namespace Styx
