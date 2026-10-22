#include "VoiceManager.hpp"
#include "AudioPipeline.hpp"
#include "Platform/Dispatcher.hpp"
#include <future>

#include "Core/Logging.hpp"
#include "Core/SettingsKeys.hpp"

#ifdef Q_OS_WIN
#include <objbase.h>
#endif

namespace Styx {
namespace Core {
namespace Audio {

VoiceManager::VoiceManager(Snowflake accountId, const ProxyConfig &proxy, QObject *parent)
    : QObject(parent), accountId(accountId), proxy(proxy)
{
    const int inVol = Settings::getInt(Settings::Keys::VoiceInputVolume, Settings::Defaults::VoiceInputVolume);
    cachedInputGain = static_cast<float>(inVol) / 100.0f;
    const int outVol = Settings::getInt(Settings::Keys::VoiceOutputVolume, Settings::Defaults::VoiceOutputVolume);
    cachedOutputVolume = static_cast<float>(outVol) / 100.0f;

    const QString inDev = Settings::getString(Settings::Keys::VoiceInputDevice, Settings::Defaults::VoiceInputDevice);
    if (!inDev.isEmpty())
        currentInputDeviceId = QByteArray::fromHex(inDev.toLatin1());
    const QString outDev = Settings::getString(Settings::Keys::VoiceOutputDevice, Settings::Defaults::VoiceOutputDevice);
    if (!outDev.isEmpty())
        currentOutputDeviceId = QByteArray::fromHex(outDev.toLatin1());
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
        if (!currentInputDeviceId.isEmpty())
            audioBackend->setInputDevice(currentInputDeviceId);
        if (!currentOutputDeviceId.isEmpty())
            audioBackend->setOutputDevice(currentOutputDeviceId);
        connect(audioBackend.get(), &IAudioBackend::devicesChanged, this, &VoiceManager::onDevicesChanged);
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
        auto oldIt = knownVoiceStates.constFind(userId);
        if (oldIt != knownVoiceStates.constEnd()) {
            hadOld = true;
            oldState = oldIt.value();
            if (!oldIt->channelId.isNull())
                oldChannel = oldIt->channelId.get();
        }

        bool leftVoice = state.channelId.isNull() || !state.channelId.get().isValid();
        Snowflake newChannel = leftVoice ? Snowflake::Invalid : state.channelId.get();

        if (leftVoice)
            knownVoiceStates.remove(userId);
        else
            knownVoiceStates[userId] = state;

        if (oldChannel != newChannel) {
            if (oldChannel.isValid())
                emit channelVoiceMemberChanged(oldChannel, userId, false);
            if (newChannel.isValid())
                emit channelVoiceMemberChanged(newChannel, userId, true);
        } else if (newChannel.isValid() && hadOld) {
            bool stateDiffers = oldState.selfMute.get() != state.selfMute.get() ||
                                oldState.selfDeaf.get() != state.selfDeaf.get() ||
                                oldState.mute.get() != state.mute.get() ||
                                oldState.deaf.get() != state.deaf.get() ||
                                oldState.suppress.get() != state.suppress.get() ||
                                oldState.selfStream.valueOr(false) != state.selfStream.valueOr(false);
            if (stateDiffers)
                emit participantVoiceStateChanged(newChannel, userId);
        }
    }

    if (isMe) {
        if (state.channelId.isNull() || !state.channelId.get().isValid()) {
            qCInfo(LogVoice) << "Voice state: disconnected from channel";
            Snowflake oldChannel = channelId;
            voiceSessionId.clear();
            channelId = Snowflake::Invalid;
            guildId = Snowflake::Invalid;
            pending = {};

            if (oldChannel.isValid())
                emit channelVoiceMemberChanged(oldChannel, accountId, false);

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
            if (!participants.isEmpty()) {
                participants.clear();
                emit participantsCleared();
            }
            populateParticipantsFromCache();

            if (oldSelfChannel.isValid())
                emit channelVoiceMemberChanged(oldSelfChannel, accountId, false);
            emit channelVoiceMemberChanged(newChannelId, accountId, true);
        }

        bool wasMuted = selfMute;
        bool wasDeaf = selfDeaf;
        selfMute = state.selfMute.get();
        selfDeaf = state.selfDeaf.get();

        qCInfo(LogVoice) << "Voice state: session =" << voiceSessionId
                         << "channel =" << channelId << "guild =" << guildId;

        if (!channelChanged && channelId.isValid() && (selfMute != wasMuted || selfDeaf != wasDeaf))
            emit participantVoiceStateChanged(channelId, accountId);

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
        if (participants.remove(userId)) {
            if (audioPipeline && voiceLoop)
                voiceLoop->post([p = audioPipeline, userId]() { p->removeUser(userId); });
            emit participantLeft(userId);
        }
    } else {
        auto it = participants.find(userId);
        bool isNew = (it == participants.end());

        VoiceParticipant &p = isNew ? participants[userId] : it.value();
        if (isNew)
            p.userId = userId;

        p.selfMute = state.selfMute.get();
        p.selfDeaf = state.selfDeaf.get();
        p.serverMute = state.mute.get();
        p.serverDeaf = state.deaf.get();
        p.suppress = state.suppress.get();
        p.streaming = state.selfStream.valueOr(false);

        if (isNew)
            emit participantJoined(userId);
        else
            emit participantUpdated(userId);
    }
}

void VoiceManager::handleVoiceServerUpdate(const Discord::VoiceServerUpdate &event)
{
    Snowflake eventGuildId = event.guildId.get();

    if (event.endpoint.isNull() || event.endpoint.get().isEmpty()) {
        qCInfo(LogVoice) << "Voice server update with null endpoint, waiting for new one";
        return;
    }

    if (!proxy.canRelayUdp()) {
        qCWarning(LogVoice) << "Refusing voice connection: an HTTP proxy cannot relay voice UDP";
        return;
    }

    QString eventEndpoint = event.endpoint.get();
    QString eventToken = event.token.get();

    qCInfo(LogVoice) << "Voice server update: guild =" << eventGuildId
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

    if (pending.hasStateUpdate && !voiceSessionId.isEmpty()) {
        connectToVoiceServer(eventEndpoint, eventToken);
        pending = {};
    }
}

void VoiceManager::disconnect()
{
    stopVoiceThread();
    pending = {};
    emit voiceDisconnected();
    emit voiceStateChanged();
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

void VoiceManager::playStreamAudio(quint32 ssrc, quint16 sequence, quint32 timestamp, const QByteArray &opus)
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
QList<AudioDeviceInfo> VoiceManager::availableInputDevices() const
{
    if (voiceLoop)
        return cachedInputDevices;
    return backend()->availableInputDevices();
}

QList<AudioDeviceInfo> VoiceManager::availableOutputDevices() const
{
    if (voiceLoop)
        return cachedOutputDevices;
    return backend()->availableOutputDevices();
}

void VoiceManager::setInputDevice(const QByteArray &deviceId)
{
    currentInputDeviceId = deviceId;
    if (audioBackend)
        audioBackend->setInputDevice(deviceId);
    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, deviceId]() { p->setInputDevice(deviceId); });
}

void VoiceManager::setOutputDevice(const QByteArray &deviceId)
{
    currentOutputDeviceId = deviceId;
    if (audioBackend)
        audioBackend->setOutputDevice(deviceId);
    if (audioPipeline && voiceLoop)
        voiceLoop->post([p = audioPipeline, deviceId]() { p->setOutputDevice(deviceId); });
}
QList<VoiceParticipant> VoiceManager::currentParticipants() const
{
    return participants.values();
}

const VoiceParticipant *VoiceManager::participant(Snowflake userId) const
{
    auto it = participants.constFind(userId);
    if (it == participants.constEnd())
        return nullptr;
    return &it.value();
}

void VoiceManager::setUserMuted(Snowflake userId, bool muted)
{
    if (muted)
        mutedUsers.insert(userId);
    else
        mutedUsers.remove(userId);

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
    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        if (!it->channelId.isNull() && it->channelId.get() == targetChannelId)
            ++count;
    }

    // include self in count
    if (channelId == targetChannelId && channelId.isValid())
        ++count;

    return count;
}

QList<Snowflake> VoiceManager::channelVoiceUsers(Snowflake targetChannelId) const
{
    QList<Snowflake> users;
    if (!targetChannelId.isValid())
        return users;

    if (channelId == targetChannelId && channelId.isValid())
        users.append(accountId);

    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        if (!it->channelId.isNull() && it->channelId.get() == targetChannelId)
            users.append(it.key());
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

    auto it = knownVoiceStates.constFind(userId);
    if (it == knownVoiceStates.constEnd())
        return std::nullopt;
    return it.value();
}

bool VoiceManager::isDaveEnabled() const
{
    return voiceClient && voiceClient->isDaveEnabled();
}

void VoiceManager::requestVerificationCode(Snowflake targetUserId, std::function<void(const QString &)> callback)
{
    if (!voiceClient) {
        callback(QString());
        return;
    }
    if (voiceLoop) {
        voiceLoop->post([vc = voiceClient, targetUserId, cb = std::move(callback)]() mutable {
            vc->requestVerificationCode(targetUserId, std::move(cb));
        });
    }
}

void VoiceManager::connectToVoiceServer(const QString &endpoint, const QString &token)
{
    if (voiceSessionId.isEmpty()) {
        qCWarning(LogVoice) << "Cannot connect to voice: no session ID";
        return;
    }

    stopVoiceThread();

    qCInfo(LogVoice) << "Creating voice loop for endpoint" << endpoint
                     << "guild" << guildId << "channel" << channelId;

    Styx::Platform::WorkerOptions opts;
    opts.name = "VoiceThread";
    opts.initComMta = true;
    voiceLoop = std::make_unique<Styx::Platform::WorkerLoop>(opts);

    Snowflake serverId = guildId.isValid() ? guildId : channelId;
    voiceClient = new Discord::Voice::VoiceClient(endpoint, token, serverId, channelId, accountId, voiceSessionId, proxy, voiceLoop.get());
    audioPipeline = new AudioPipeline;

    QList<Snowflake> channelUsers;
    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        const auto &vs = it.value();
        if (!vs.channelId.isNull() && vs.channelId.get() == channelId)
            channelUsers.append(it.key());
    }
    voiceClient->seedConnectedUsers(channelUsers);

    cachedInputDevices = backend()->availableInputDevices();
    cachedOutputDevices = backend()->availableOutputDevices();

    connect(voiceClient, &Discord::Voice::VoiceClient::audioReceived, audioPipeline, &AudioPipeline::onAudioReceived, Qt::DirectConnection);
    connect(audioPipeline, &AudioPipeline::encodedAudioReady, voiceClient, &Discord::Voice::VoiceClient::sendAudio, Qt::DirectConnection);
    connect(audioPipeline, &AudioPipeline::speakingChanged, voiceClient, &Discord::Voice::VoiceClient::setSpeaking, Qt::DirectConnection);

    connect(voiceClient, &Discord::Voice::VoiceClient::speakingReceived, audioPipeline,
            [ap = audioPipeline](const Discord::Voice::SpeakingData &data) {
                if (data.userId.hasValue() && data.userId->isValid() && data.ssrc.get() != 0)
                    ap->setSsrcUserId(data.ssrc, data.userId.get());
            }, Qt::DirectConnection);

    connect(voiceClient, &Discord::Voice::VoiceClient::clientConnected, audioPipeline,
            [ap = audioPipeline](const Discord::Voice::ClientConnectData &data) {
                if (data.userId.hasValue() && data.userId->isValid() && data.audioSsrc.get() != 0)
                    ap->setSsrcUserId(data.audioSsrc, data.userId.get());
            }, Qt::DirectConnection);

    unsigned int gen = voiceGeneration;
    auto alive = m_voiceAlive;

    connect(voiceClient, &Discord::Voice::VoiceClient::clientConnected,
            this, [this, gen, alive](const Discord::Voice::ClientConnectData &data) {
                Styx::Platform::Dispatcher::main().post([this, gen, alive, data]() {
                    if (!*alive || gen != voiceGeneration)
                        return;
                    Snowflake userId = data.userId;
                    if (!userId.isValid() || userId == accountId || participants.contains(userId))
                        return;
                    VoiceParticipant p;
                    p.userId = userId;
                    participants.insert(userId, p);
                    emit participantJoined(userId);
                });
            }, Qt::DirectConnection);

    connect(voiceClient, &Discord::Voice::VoiceClient::clientDisconnected,
            this, [this, gen, alive](Snowflake userId) {
                Styx::Platform::Dispatcher::main().post([this, gen, alive, userId]() {
                    if (!*alive || gen != voiceGeneration)
                        return;
                    if (participants.remove(userId)) {
                        if (audioPipeline && voiceLoop)
                            voiceLoop->post([p = audioPipeline, userId]() { p->removeUser(userId); });
                        emit participantLeft(userId);
                    }
                });
            }, Qt::DirectConnection);

    connect(voiceClient, &Discord::Voice::VoiceClient::speakingReceived,
            this, [this, gen, alive](const Discord::Voice::SpeakingData &data) {
                Styx::Platform::Dispatcher::main().post([this, gen, alive, data]() {
                    if (!*alive || gen != voiceGeneration)
                        return;
                    if (!data.userId.hasValue() || !data.userId->isValid())
                        return;
                    Snowflake userId = data.userId.get();
                    auto it = participants.find(userId);
                    if (it == participants.end())
                        return;
                    bool wasSpeaking = it->speaking;
                    it->speaking = (data.speaking.get() != 0);
                    if (it->speaking != wasSpeaking)
                        emit participantSpeakingChanged(userId, it->speaking);
                });
            }, Qt::DirectConnection);

    connect(audioPipeline, &AudioPipeline::userAudioLevelChanged, this, [this, alive](Snowflake userId, float level) {
        Styx::Platform::Dispatcher::main().post([this, alive, userId, level]() {
            if (!*alive)
                return;
            emit userAudioLevelChanged(userId, level);
        });
    }, Qt::DirectConnection);

    connect(voiceClient, &Discord::Voice::VoiceClient::connected,
            this, [this, gen, alive]() {
                Styx::Platform::Dispatcher::main().post([this, gen, alive]() {
                    if (!*alive || gen != voiceGeneration)
                        return;
                    onVoiceClientConnected();
                });
            }, Qt::DirectConnection);

    connect(voiceClient, &Discord::Voice::VoiceClient::disconnected, this, [this, gen, alive]() {
        Styx::Platform::Dispatcher::main().post([this, gen, alive]() {
            if (!*alive || gen != voiceGeneration)
                return;
            onVoiceClientDisconnected();
        });
    }, Qt::DirectConnection);

    connect(voiceClient, &Discord::Voice::VoiceClient::stateChanged,
            this, [this, gen, alive](Discord::Voice::VoiceClient::State state) {
                Styx::Platform::Dispatcher::main().post([this, gen, alive, state]() {
                    if (!*alive || gen != voiceGeneration)
                        return;
                    onVoiceClientStateChanged(state);
                });
            }, Qt::DirectConnection);

    connect(audioPipeline, &AudioPipeline::audioLevelChanged, this, [this, alive](float level) {
        Styx::Platform::Dispatcher::main().post([this, alive, level]() {
            if (!*alive)
                return;
            emit audioLevelChanged(level);
        });
    }, Qt::DirectConnection);

    connect(audioPipeline, &AudioPipeline::speakingChanged, this, [this, alive](bool speaking) {
        Styx::Platform::Dispatcher::main().post([this, alive, speaking]() {
            if (!*alive)
                return;
            emit speakingChanged(speaking);
        });
    }, Qt::DirectConnection);

    connect(voiceClient, &Discord::Voice::VoiceClient::privacyCodeChanged,
            this, [this, gen, alive](const QString &code) {
                Styx::Platform::Dispatcher::main().post([this, gen, alive, code]() {
                    if (!*alive || gen != voiceGeneration)
                        return;
                    cachedPrivacyCode = code;
                    emit privacyCodeChanged(code);
                });
            }, Qt::DirectConnection);

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
    m_voiceAlive = std::make_shared<bool>(true);

    bool hadParticipants = !participants.isEmpty();
    participants.clear();
    mutedUsers.clear();
    cachedPrivacyCode.clear();
    if (hadParticipants)
        emit participantsCleared();

    AudioPipeline *ap = audioPipeline;
    Discord::Voice::VoiceClient *vc = voiceClient;

    if (voiceLoop->isRunning()) {
        std::promise<void> promise;
        auto future = promise.get_future();
        voiceLoop->post([ap, vc, &promise]() {
            if (ap) ap->stop();
            if (vc) vc->stop();
            promise.set_value();
        });
        if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            qCWarning(LogVoice) << "Voice worker stop timed out";
        }
    }

    voiceLoop->stopAndWait();
    voiceLoop.reset();

    delete audioPipeline;
    audioPipeline = nullptr;

    delete voiceClient;
    voiceClient = nullptr;

    qCDebug(LogVoice) << "Voice loop stopped";
}

void VoiceManager::populateParticipantsFromCache()
{
    for (auto it = knownVoiceStates.constBegin(); it != knownVoiceStates.constEnd(); ++it) {
        const auto &vs = it.value();
        if (vs.channelId.isNull() || vs.channelId.get() != channelId)
            continue;
        if (participants.contains(it.key()))
            continue;

        VoiceParticipant p;
        p.userId = it.key();
        p.selfMute = vs.selfMute.get();
        p.selfDeaf = vs.selfDeaf.get();
        p.serverMute = vs.mute.get();
        p.serverDeaf = vs.deaf.get();
        p.suppress = vs.suppress.get();
        p.streaming = vs.selfStream.valueOr(false);
        participants.insert(it.key(), p);
        emit participantJoined(it.key());
    }
}

void VoiceManager::onVoiceClientConnected()
{
    if (!audioPipeline)
        return;

    qCInfo(LogVoice) << "Voice connection established for channel" << channelId;

    bool capturing = !selfMute && !selfDeaf;
    QByteArray inputId = currentInputDeviceId;
    QByteArray outputId = currentOutputDeviceId;
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
            if (!inputId.isEmpty())
                p->setInputDevice(inputId);
            if (!outputId.isEmpty())
                p->setOutputDevice(outputId);
        });
    }

    populateParticipantsFromCache();

    emit voiceConnected();
    emit voiceStateChanged();
}

void VoiceManager::onVoiceClientDisconnected()
{
    qCInfo(LogVoice) << "Voice client disconnected";

    stopVoiceThread();

    emit voiceDisconnected();
    emit voiceStateChanged();
}

void VoiceManager::onVoiceClientStateChanged(Discord::Voice::VoiceClient::State state)
{
    qCDebug(LogVoice) << "VoiceManager: client state ->" << static_cast<int>(state);
    emit voiceStateChanged();
}

void VoiceManager::onDevicesChanged(const QList<AudioDeviceInfo> &inputs, const QList<AudioDeviceInfo> &outputs)
{
    cachedInputDevices = inputs;
    cachedOutputDevices = outputs;
    emit devicesChanged();
}

} // namespace Audio
} // namespace Core
} // namespace Styx
