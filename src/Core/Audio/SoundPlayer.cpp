#include "Sounds.hpp"

#ifdef STYX_HAVE_MINIAUDIO

#include "Core/Audio/AudioBackends.hpp"
#include "Core/Audio/Miniaudio.hpp"
#include "Core/Logging.hpp"
#include "Platform/Dispatcher.hpp"
#include "Platform/Settings.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace Styx::Core::Sounds {
namespace {

constexpr std::size_t MAX_VOICES = 4;
// The same sound again this soon is dropped, so a burst of messages doesn't stutter.
constexpr auto REPEAT_GAP = std::chrono::milliseconds(250);
// The device is stopped once nothing has played for this long, so Styx doesn't hold it open.
constexpr auto IDLE_STOP = std::chrono::seconds(3);
constexpr std::size_t SOUND_COUNT = 5;

struct Voice
{
    std::shared_ptr<const std::vector<float>> samples;
    std::size_t position = 0;
};

class SoundPlayer
{
public:
    static SoundPlayer &instance()
    {
        // Never destroyed: the audio callback may still run while other statics are torn down.
        static SoundPlayer &player = *new SoundPlayer;
        return player;
    }

    void play(Sound sound);

private:
    SoundPlayer() = default;
    bool ensureRunning();
    void stopIfIdle();
    void armIdleTimer();
    static void onPlayback(ma_device *device, void *output, const void *, ma_uint32 frameCount);
    void mix(float *output, ma_uint32 frameCount);

    std::array<std::shared_ptr<const std::vector<float>>, SOUND_COUNT> cache;
    std::array<std::chrono::steady_clock::time_point, SOUND_COUNT> lastPlayed{};

    std::mutex mixMutex;
    std::vector<Voice> voices;

    ma_device device = {};
    bool deviceInit = false;
    bool deviceRunning = false;
    Platform::TimerId idleTimer = 0;
};

void SoundPlayer::play(Sound sound)
{
    const auto index = static_cast<std::size_t>(sound);
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPlayed[index] < REPEAT_GAP)
        return;
    lastPlayed[index] = now;

    if (!cache[index])
        cache[index] = std::make_shared<const std::vector<float>>(synthesize(sound));

    if (!ensureRunning())
        return;

    {
        std::lock_guard lock(mixMutex);
        if (voices.size() >= MAX_VOICES)
            voices.erase(voices.begin());
        voices.push_back({cache[index], 0});
    }

    armIdleTimer();
}

void SoundPlayer::armIdleTimer()
{
    auto &dispatcher = Platform::Dispatcher::main();
    if (idleTimer)
        dispatcher.cancelTimer(idleTimer);
    idleTimer = dispatcher.setTimer(std::chrono::duration_cast<std::chrono::milliseconds>(IDLE_STOP), false, [this] {
        idleTimer = 0;
        stopIfIdle();
    });
}

bool SoundPlayer::ensureRunning()
{
    if (!deviceInit) {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 1;
        config.sampleRate = SAMPLE_RATE;
        config.dataCallback = &SoundPlayer::onPlayback;
        config.pUserData = this;

        if (!Audio::initAudioDevice(&config, &device)) {
            Core::log(Core::LogLevel::Warning, LogVoice) << "failed to open the device for sounds";
            return false;
        }
        deviceInit = true;
    }

    if (deviceRunning)
        return true;

    if (ma_device_start(&device) != MA_SUCCESS) {
        Core::log(Core::LogLevel::Warning, LogVoice) << "failed to start the device for sounds";
        return false;
    }

    deviceRunning = true;
    return true;
}

void SoundPlayer::stopIfIdle()
{
    bool idle = false;
    {
        std::lock_guard lock(mixMutex);
        idle = voices.empty();
    }
    if (!idle) {
        armIdleTimer();
        return;
    }
    if (deviceRunning) {
        ma_device_stop(&device);
        deviceRunning = false;
    }
}

void SoundPlayer::onPlayback(ma_device *device, void *output, const void *, ma_uint32 frameCount)
{
    static_cast<SoundPlayer *>(device->pUserData)->mix(static_cast<float *>(output), frameCount);
}

void SoundPlayer::mix(float *output, ma_uint32 frameCount)
{
    std::fill(output, output + frameCount, 0.0f);
    std::lock_guard lock(mixMutex);
    for (auto &voice : voices) {
        const auto &samples = *voice.samples;
        const std::size_t count = std::min<std::size_t>(frameCount, samples.size() - voice.position);
        for (std::size_t i = 0; i < count; ++i)
            output[i] += samples[voice.position + i];
        voice.position += count;
    }
    voices.erase(std::remove_if(voices.begin(), voices.end(), [](const Voice &voice) {
        return voice.position >= voice.samples->size();
    }), voices.end());
    for (ma_uint32 i = 0; i < frameCount; ++i)
        output[i] = std::clamp(output[i], -1.0f, 1.0f);
}

} // namespace

void play(Sound sound)
{
    if (!Platform::Settings::instance().getBool(settingKey(sound), true))
        return;
    SoundPlayer::instance().play(sound);
}

} // namespace Styx::Core::Sounds

#else

namespace Styx::Core::Sounds {

// Built without an audio backend.
void play(Sound) {}

} // namespace Styx::Core::Sounds

#endif
