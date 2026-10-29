#pragma once

#include "IAudioBackend.hpp"

#include <atomic>
#include <memory>

struct ma_device;

namespace Styx {
namespace Core {
namespace Audio {

struct MiniaudioState;

class MiniaudioAudioBackend : public IAudioBackend
{
public:
    MiniaudioAudioBackend();
    ~MiniaudioAudioBackend() override;

    std::vector<AudioDeviceInfo> availableInputDevices() const override;
    std::vector<AudioDeviceInfo> availableOutputDevices() const override;

    [[nodiscard]] std::string currentInputDevice() const override { return selectedInputId; }
    [[nodiscard]] std::string currentOutputDevice() const override { return selectedOutputId; }
    void setInputDevice(const std::string &deviceId) override;
    void setOutputDevice(const std::string &deviceId) override;

    bool startCapture() override;
    void stopCapture() override;
    bool startPlayback() override;
    void stopPlayback() override;

    [[nodiscard]] bool isCapturing() const override;
    [[nodiscard]] bool isPlaying() const override;
    [[nodiscard]] int nativeCaptureChannels() const override;

    void setInputGain(float gain) override;
    void setOutputVolume(float volume) override;

    bool pushPlaybackFrame(const int16_t *frame) override;

private:
    // audio thread callbacks
    void handleCapturedFrames(const void *input, unsigned int frameCount);
    void handlePlaybackFrames(void *output, unsigned int frameCount);

    friend void OnCapture(ma_device *, void *, const void *, uint32_t);
    friend void OnPlayback(ma_device *, void *, const void *, uint32_t);

private:
    std::unique_ptr<MiniaudioState> ma;

    std::string captureBuffer;

    std::string selectedInputId;
    std::string selectedOutputId;

    std::atomic<float> inputGain{ 1.0f };
    std::atomic<float> outputVolume{ 1.0f };
};

} // namespace Audio
} // namespace Core
} // namespace Styx
