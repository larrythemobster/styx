#pragma once

#include "Platform/Trackable.hpp"
#include "Platform/WorkerLoop.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <opus.h>

#include "Core/Snowflake.hpp"
#include "Core/Audio/TransmitGate.hpp"


namespace Styx {
namespace Core {
namespace Audio {

class IAudioBackend;
class OpusEncoder;
class OpusDecoder;
class JitterBuffer;
class NoiseSuppressor;

struct SpeakerState
{
    std::unique_ptr<OpusDecoder> decoder;
    std::unique_ptr<JitterBuffer> jitterBuffer;
    std::deque<std::string> pendingFrames; // overflow from multi-frame Opus packets
};

class AudioPipeline : public Platform::Trackable
{
public:
    explicit AudioPipeline(Platform::WorkerLoop &loop);
    ~AudioPipeline() override;

public:
    void start(IAudioBackend *backend, bool capturing);
    void stop();

    void startCapture();
    void stopCapture();

    void onAudioReceived(uint32_t ssrc, uint16_t sequence, uint32_t timestamp, const std::string &opusData);

    void setSsrcUserId(uint32_t ssrc, Snowflake userId);
    void removeUser(Snowflake userId);
    void setDeafened(bool deafened);
    void setUserVolume(Snowflake userId, float volume);
    void setInputDevice(const std::string &deviceId);
    void setOutputDevice(const std::string &deviceId);
    void setInputGain(float gain);
    void setOutputVolume(float volume);
    void setVadThreshold(float threshold);
    void setPushToTalk(bool enabled, int releaseDelayMs);
    void setPushToTalkHeld(bool held);
    void setNoiseSuppressionEnabled(bool enabled);
    void setUseRnnoiseVad(bool enabled);

    void setOpusApplication(int application);
    void setOpusBitrate(int bitrate);
    void setOpusComplexity(int complexity);
    void setOpusSignalType(int signalType);
    void setOpusFec(bool enabled);
    void setOpusPacketLossPercent(int percent);

    Platform::Signal<std::string> encodedAudioReady;
    Platform::Signal<bool> speakingChanged;
    Platform::Signal<float> audioLevelChanged;
    Platform::Signal<Snowflake, float> userAudioLevelChanged;

private:
    void onAudioCaptured(const std::string &pcmData);
    void onMixTick();

private:
    bool detectVoiceActivity(const std::string &pcmFrame, float &outRms) const;
    void sendTrailingSilence();
    void initializeEncoder();
    void reconfigureNoiseSuppressorChannels();
    static float computeRms(const int16_t *samples, int count);

    Platform::WorkerLoop &loop;
    Platform::TimerId mixTimer = 0;
    Platform::Connection captureConnection;
    std::shared_ptr<std::atomic<bool>> captureActive;
    IAudioBackend *audioBackend = nullptr;
    std::unique_ptr<OpusEncoder> encoder;
    std::unique_ptr<NoiseSuppressor> noiseSuppressor;
    std::unordered_map<uint32_t, SpeakerState> speakers;

    std::unordered_map<uint32_t, Snowflake> ssrcToUser;
    std::unordered_map<Snowflake, float> userVolumes;

    bool deafened = false;
    TransmitGate gate;
    float vadThreshold = 100.0f;

    bool noiseSuppressionEnabled = true;
#ifdef STYX_HAVE_RNNOISE
    bool useRnnoiseVad = true;
#else
    bool useRnnoiseVad = false;
#endif
    float vadProbabilityThreshold = 0.5f;

    int opusApplication = OPUS_APPLICATION_VOIP;
    int opusBitrate = 64000;
    int opusComplexity = 5;
    int opusSignalType = OPUS_SIGNAL_VOICE;
    bool opusFec = true;
    int opusPacketLossPercent = 0;

    std::chrono::steady_clock::time_point rmsThrottleTimer;
    std::chrono::steady_clock::time_point userRmsThrottleTimer;
    std::unordered_map<Snowflake, float> pendingUserRms;

    static constexpr int TRAILING_SILENCE_FRAMES = 5;
    static constexpr int RMS_EMIT_INTERVAL_MS = 60;
};

} // namespace Audio
} // namespace Core
} // namespace Styx
