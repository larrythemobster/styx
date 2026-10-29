#pragma once

#include "Platform/Trackable.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Styx {
namespace Core {
namespace Audio {

inline constexpr uint8_t OPUS_SILENCE[] = { 0xF8, 0xFF, 0xFE };

inline constexpr int AUDIO_SAMPLE_RATE = 48000;
inline constexpr int AUDIO_CHANNELS = 2;
inline constexpr int AUDIO_SAMPLE_BYTES = 2;
inline constexpr int AUDIO_FRAME_DURATION_MS = 20;
inline constexpr int AUDIO_FRAME_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_FRAME_DURATION_MS / 1000; // 960
inline constexpr int AUDIO_FRAME_SIZE = AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS * AUDIO_SAMPLE_BYTES; // 3840 bytes

struct AudioDeviceInfo
{
    std::string id;
    std::string description;
    bool isDefault = false;
};

class IAudioBackend : public Platform::Trackable
{
public:
    ~IAudioBackend() override = default;

    static std::unique_ptr<IAudioBackend> create();

    virtual std::vector<AudioDeviceInfo> availableInputDevices() const = 0;
    virtual std::vector<AudioDeviceInfo> availableOutputDevices() const = 0;

    [[nodiscard]] virtual std::string currentInputDevice() const = 0;
    [[nodiscard]] virtual std::string currentOutputDevice() const = 0;
    virtual void setInputDevice(const std::string &deviceId) = 0;
    virtual void setOutputDevice(const std::string &deviceId) = 0;

    virtual bool startCapture() = 0;
    virtual void stopCapture() = 0;
    virtual bool startPlayback() = 0;
    virtual void stopPlayback() = 0;

    [[nodiscard]] virtual bool isCapturing() const = 0;
    [[nodiscard]] virtual bool isPlaying() const = 0;

    [[nodiscard]] virtual int nativeCaptureChannels() const = 0;

    virtual void setInputGain(float gain) = 0;
    virtual void setOutputVolume(float volume) = 0;

    virtual bool pushPlaybackFrame(const int16_t *frame) = 0;

    Platform::Signal<std::string> audioCaptured;
    Platform::Signal<std::vector<AudioDeviceInfo>, std::vector<AudioDeviceInfo>> devicesChanged;
};

} // namespace Audio
} // namespace Core
} // namespace Styx
