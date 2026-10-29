#include "AudioPipeline.hpp"
#include "IAudioBackend.hpp"
#include "OpusEncoder.hpp"
#include "OpusDecoder.hpp"
#include "JitterBuffer.hpp"
#include "AudioMixer.hpp"
#include "NoiseSuppressor.hpp"

#include "Core/Logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Styx {
namespace Core {
namespace Audio {

AudioPipeline::AudioPipeline(Platform::WorkerLoop &loop)
    : loop(loop)
{
}

AudioPipeline::~AudioPipeline()
{
    stop();
}

void AudioPipeline::start(IAudioBackend *backend, bool capturing)
{
    if (audioBackend)
        return;

    audioBackend = backend;
    captureActive = std::make_shared<std::atomic<bool>>(true);
    auto active = captureActive;
    auto weak = lifetime();
    auto *worker = &loop;
    captureConnection = audioBackend->audioCaptured.connect([this, worker, active, weak](std::string frame) {
        worker->post([this, active, weak, frame = std::move(frame)] {
            if (active->load() && !weak.expired())
                onAudioCaptured(frame);
        });
    });

    initializeEncoder();

    noiseSuppressor = std::make_unique<NoiseSuppressor>();
    if (!noiseSuppressor->init())
        noiseSuppressor.reset();

    rmsThrottleTimer = std::chrono::steady_clock::now();
    userRmsThrottleTimer = rmsThrottleTimer;

    if (capturing)
        audioBackend->startCapture();
    reconfigureNoiseSuppressorChannels();

    audioBackend->startPlayback();

    mixTimer = loop.setTimer(AUDIO_FRAME_DURATION_MS, true, [this] { onMixTick(); });

    log(LogLevel::Info, LogVoice) << "Audio pipeline started";
}

void AudioPipeline::stop()
{
    if (mixTimer)
        loop.cancelTimer(std::exchange(mixTimer, 0));
    if (captureActive)
        captureActive->store(false);
    captureConnection.disconnect();
    captureActive.reset();

    if (audioBackend) {
        audioBackend->stopCapture();
        audioBackend->stopPlayback();
    }

    speakers.clear();
    ssrcToUser.clear();
    pendingUserRms.clear();

    encoder.reset();
    noiseSuppressor.reset();

    audioBackend = nullptr;

    isSpeaking = false;
    vadHoldoffCounter = 0;

    log(LogLevel::Debug, LogVoice) << "Audio pipeline stopped";
}

void AudioPipeline::startCapture()
{
    if (!audioBackend)
        return;

    audioBackend->startCapture();
    reconfigureNoiseSuppressorChannels();
}

void AudioPipeline::stopCapture()
{
    if (!audioBackend)
        return;

    audioBackend->stopCapture();

    if (isSpeaking) {
        sendTrailingSilence();
        isSpeaking = false;
        vadHoldoffCounter = 0;
        speakingChanged.publish(false);
    }
}

void AudioPipeline::onAudioReceived(uint32_t ssrc, uint16_t sequence, uint32_t /*timestamp*/, const std::string &opusData)
{
    auto it = speakers.find(ssrc);
    if (it == speakers.end()) {
        SpeakerState state;
        state.decoder = std::make_unique<OpusDecoder>();
        if (!state.decoder->init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS)) {
            log(LogLevel::Warning, LogVoice) << "Failed to init decoder for SSRC" << ssrc;
            return;
        }
        state.jitterBuffer = std::make_unique<JitterBuffer>();

        auto [inserted, _] = speakers.emplace(ssrc, std::move(state));
        it = inserted;
    }

    it->second.jitterBuffer->push(sequence, opusData);
}

void AudioPipeline::setDeafened(bool deafened)
{
    this->deafened = deafened;
}

void AudioPipeline::setSsrcUserId(uint32_t ssrc, Snowflake userId)
{
    ssrcToUser[ssrc] = userId;
}

void AudioPipeline::removeUser(Snowflake userId)
{
    for (auto it = ssrcToUser.begin(); it != ssrcToUser.end();) {
        if (it->second == userId) {
            speakers.erase(it->first);
            it = ssrcToUser.erase(it);
        } else {
            ++it;
        }
    }
    userVolumes.erase(userId);
    pendingUserRms.erase(userId);
}

void AudioPipeline::setUserVolume(Snowflake userId, float volume)
{
    if (volume == 1.0f)
        userVolumes.erase(userId);
    else
        userVolumes[userId] = volume;
}

void AudioPipeline::setInputDevice(const std::string &deviceId)
{
    if (!audioBackend)
        return;

    audioBackend->setInputDevice(deviceId);
    reconfigureNoiseSuppressorChannels();
}

void AudioPipeline::reconfigureNoiseSuppressorChannels()
{
    if (noiseSuppressor && audioBackend && audioBackend->isCapturing())
        noiseSuppressor->reconfigure(audioBackend->nativeCaptureChannels());
}

void AudioPipeline::setOutputDevice(const std::string &deviceId)
{
    if (audioBackend)
        audioBackend->setOutputDevice(deviceId);
}

void AudioPipeline::setInputGain(float gain)
{
    if (audioBackend)
        audioBackend->setInputGain(gain);
}

void AudioPipeline::setOutputVolume(float volume)
{
    if (audioBackend)
        audioBackend->setOutputVolume(volume);
}

void AudioPipeline::setVadThreshold(float threshold)
{
    vadThreshold = threshold;
}

void AudioPipeline::setNoiseSuppressionEnabled(bool enabled)
{
    noiseSuppressionEnabled = enabled;
}

void AudioPipeline::setUseRnnoiseVad(bool enabled)
{
    useRnnoiseVad = enabled;
}

void AudioPipeline::initializeEncoder()
{
    encoder = std::make_unique<OpusEncoder>();
    if (!encoder->init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, opusApplication)) {
        log(LogLevel::Error, LogVoice) << "Failed to initialize Opus encoder";
        encoder.reset();
        return;
    }
    encoder->setBitrate(opusBitrate);
    encoder->setComplexity(opusComplexity);
    encoder->setSignalType(opusSignalType);
    encoder->setFec(opusFec);
    encoder->setPacketLossPercent(opusPacketLossPercent);
}

void AudioPipeline::setOpusApplication(int application)
{
    if (opusApplication == application)
        return;

    opusApplication = application;

    if (encoder)
        initializeEncoder();
}

void AudioPipeline::setOpusBitrate(int bitrate)
{
    opusBitrate = bitrate;

    if (encoder)
        encoder->setBitrate(bitrate);
}

void AudioPipeline::setOpusComplexity(int complexity)
{
    opusComplexity = complexity;

    if (encoder)
        encoder->setComplexity(complexity);
}

void AudioPipeline::setOpusSignalType(int signalType)
{
    opusSignalType = signalType;

    if (encoder)
        encoder->setSignalType(signalType);
}

void AudioPipeline::setOpusFec(bool enabled)
{
    opusFec = enabled;

    if (encoder)
        encoder->setFec(enabled);
}

void AudioPipeline::setOpusPacketLossPercent(int percent)
{
    opusPacketLossPercent = percent;

    if (encoder)
        encoder->setPacketLossPercent(percent);
}

void AudioPipeline::onAudioCaptured(const std::string &pcmData)
{
    if (!encoder)
        return;

    std::string frame = pcmData;
    float voiceProb = -1.0f;
    if (noiseSuppressor && (noiseSuppressionEnabled || useRnnoiseVad)) {
        std::string denoised = noiseSuppressor->process(pcmData, voiceProb);
        if (noiseSuppressionEnabled)
            frame = std::move(denoised);
    }

    float rms = 0.0f;
    bool rmsVoice = detectVoiceActivity(frame, rms);

    bool voiceDetected;
    if (useRnnoiseVad && voiceProb >= 0.0f)
        voiceDetected = voiceProb > vadProbabilityThreshold;
    else
        voiceDetected = rmsVoice;

    auto now = std::chrono::steady_clock::now();
    if (now - rmsThrottleTimer >= std::chrono::milliseconds(RMS_EMIT_INTERVAL_MS)) {
        audioLevelChanged.publish(rms);
        rmsThrottleTimer = now;
    }

    if (voiceDetected) {
        vadHoldoffCounter = vadHoldoffFrames;
        if (!isSpeaking) {
            isSpeaking = true;
            speakingChanged.publish(true);
        }
    } else if (vadHoldoffCounter > 0) {
        vadHoldoffCounter--;
    } else if (isSpeaking) {
        sendTrailingSilence();
        isSpeaking = false;
        speakingChanged.publish(false);
        return;
    }

    if (!isSpeaking)
        return;

    std::string encoded = encoder->encode(frame);
    if (!encoded.empty())
        encodedAudioReady.publish(std::move(encoded));
}

void AudioPipeline::onMixTick()
{
    if (deafened || !audioBackend)
        return;

    std::vector<std::pair<std::string, float>> streams;

    for (auto it = speakers.begin(); it != speakers.end(); ++it) {
        uint32_t ssrc = it->first;
        SpeakerState &state = it->second;

        std::string pcm;

        if (!state.pendingFrames.empty()) {
            pcm = std::move(state.pendingFrames.front());
            state.pendingFrames.pop_front();
        } else {
            if (!state.jitterBuffer->isReady())
                continue;

            std::string opusData = state.jitterBuffer->pop();

            if (opusData.empty()) {
                pcm = state.decoder->decodePlc();
            } else {
                std::vector<std::string> frames = state.decoder->decode(opusData);
                if (frames.empty())
                    continue;
                pcm = std::move(frames.front());
                for (size_t i = 1; i < frames.size(); i++)
                    state.pendingFrames.push_back(std::move(frames[i]));
            }
        }

        if (pcm.empty())
            continue;

        // rms before gain
        auto userIt = ssrcToUser.find(ssrc);
        Snowflake userId;
        if (userIt != ssrcToUser.end())
            userId = userIt->second;

        if (userId.isValid()) {
            const auto *samples = reinterpret_cast<const int16_t *>(pcm.data());
            int count = pcm.size() / static_cast<int>(sizeof(int16_t));
            if (count > 0) {
                float rms = computeRms(samples, count);

                auto rmsIt = pendingUserRms.find(userId);
                if (rmsIt == pendingUserRms.end())
                    pendingUserRms.emplace(userId, rms);
                else if (rms > rmsIt->second)
                    rmsIt->second = rms;
            }
        }

        float gain = 1.0f;
        if (userId.isValid()) {
            auto volume = userVolumes.find(userId);
            if (volume != userVolumes.end())
                gain = volume->second;
        }

        streams.emplace_back(std::move(pcm), gain);
    }

    // emit periodically
    auto now = std::chrono::steady_clock::now();
    if (now - userRmsThrottleTimer >= std::chrono::milliseconds(RMS_EMIT_INTERVAL_MS)) {
        for (const auto &[userId, rms] : pendingUserRms)
            userAudioLevelChanged.publish(userId, rms);
        pendingUserRms.clear();
        userRmsThrottleTimer = now;
    }

    if (streams.empty())
        return;

    std::string mixed = AudioMixer::mix(streams);
    if (mixed.empty())
        return;

    audioBackend->pushPlaybackFrame(reinterpret_cast<const int16_t *>(mixed.data()));
}

bool AudioPipeline::detectVoiceActivity(const std::string &pcmFrame, float &outRms) const
{
    const auto *samples = reinterpret_cast<const int16_t *>(pcmFrame.data());
    int count = pcmFrame.size() / static_cast<int>(sizeof(int16_t));
    if (count == 0) {
        outRms = 0.0f;
        return false;
    }

    outRms = computeRms(samples, count);
    return outRms > vadThreshold;
}

float AudioPipeline::computeRms(const int16_t *samples, int count)
{
    double sum = 0;
    for (int i = 0; i < count; i++)
        sum += static_cast<double>(samples[i]) * samples[i];
    return static_cast<float>(std::sqrt(sum / count));
}

void AudioPipeline::sendTrailingSilence()
{
    std::string silence(reinterpret_cast<const char *>(OPUS_SILENCE), sizeof(OPUS_SILENCE));
    for (int i = 0; i < TRAILING_SILENCE_FRAMES; i++)
        encodedAudioReady.publish(silence);
}

} // namespace Audio
} // namespace Core
} // namespace Styx
