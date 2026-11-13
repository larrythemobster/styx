#include "Sounds.hpp"

#ifdef STYX_HAVE_MINIAUDIO

#include "SoundMixer.hpp"
#include "Core/Audio/AudioBackends.hpp"
#include "Core/Audio/Miniaudio.hpp"
#include "Core/Logging.hpp"
#include "Core/SettingsKeys.hpp"
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
    void startLoop(Sound sound);
    void stopLoop(Sound sound);
    void stopAll();

private:
    SoundPlayer() = default;
    std::shared_ptr<const std::vector<float>> samplesFor(Sound sound);
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

std::shared_ptr<const std::vector<float>> SoundPlayer::samplesFor(Sound sound)
{
    const auto index = static_cast<std::size_t>(sound);
    if (!cache[index])
        cache[index] = std::make_shared<const std::vector<float>>(synthesize(sound));
    return cache[index];
}

void SoundPlayer::play(Sound sound)
{
    const auto index = static_cast<std::size_t>(sound);
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPlayed[index] < REPEAT_GAP)
        return;
    lastPlayed[index] = now;

    auto samples = samplesFor(sound);
    if (!ensureRunning())
        return;

    {
        std::lock_guard lock(mixMutex);
        if (voices.size() >= MAX_VOICES) {
            const auto oldest = std::find_if(voices.begin(), voices.end(), [](const Voice &voice) { return !voice.looping; });
            if (oldest == voices.end())
                return;
            voices.erase(oldest);
        }
        voices.push_back({sound, std::move(samples), 0, false});
    }

    armIdleTimer();
}

void SoundPlayer::startLoop(Sound sound)
{
    auto samples = samplesFor(sound);
    if (!ensureRunning())
        return;
    {
        std::lock_guard lock(mixMutex);
        if (std::any_of(voices.begin(), voices.end(), [sound](const Voice &voice) { return voice.looping && voice.sound == sound; }))
            return;
        voices.push_back({sound, std::move(samples), 0, true});
    }
    armIdleTimer();
}

void SoundPlayer::stopLoop(Sound sound)
{
    std::lock_guard lock(mixMutex);
    voices.erase(std::remove_if(voices.begin(), voices.end(), [sound](const Voice &voice) { return voice.looping && voice.sound == sound; }), voices.end());
}

void SoundPlayer::stopAll()
{
    {
        std::lock_guard lock(mixMutex);
        voices.clear();
    }
    stopIfIdle();
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
    std::lock_guard lock(mixMutex);
    mixVoices(voices, output, frameCount);
}

} // namespace

void play(Sound sound)
{
    if (!Platform::Settings::instance().getBool(Core::Settings::Keys::SoundsEnabled, Core::Settings::Defaults::SoundsEnabled) ||
        !Platform::Settings::instance().getBool(settingKey(sound), true))
        return;
    SoundPlayer::instance().play(sound);
}

void startLoop(Sound sound)
{
    if (!Platform::Settings::instance().getBool(Core::Settings::Keys::SoundsEnabled, Core::Settings::Defaults::SoundsEnabled) ||
        !Platform::Settings::instance().getBool(settingKey(sound), true))
        return;
    SoundPlayer::instance().startLoop(sound);
}

void stopLoop(Sound sound)
{
    SoundPlayer::instance().stopLoop(sound);
}

void preview(Sound sound)
{
    if (Platform::Settings::instance().getBool(Core::Settings::Keys::SoundsEnabled, Core::Settings::Defaults::SoundsEnabled))
        SoundPlayer::instance().play(sound);
}

void stopAll()
{
    SoundPlayer::instance().stopAll();
}

} // namespace Styx::Core::Sounds

#else

namespace Styx::Core::Sounds {

// Built without an audio backend.
void play(Sound) {}
void startLoop(Sound) {}
void stopLoop(Sound) {}

} // namespace Styx::Core::Sounds

#endif
