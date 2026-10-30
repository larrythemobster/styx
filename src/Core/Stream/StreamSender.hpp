#pragma once

#include "Platform/Trackable.hpp"
#include "Platform/WorkerLoop.hpp"
#include <chrono>
#include <string>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "ScreenCapture.hpp"
#include "StreamQuality.hpp"

struct AVFrame;

namespace Styx {
namespace Core {
namespace Audio {
class OpusEncoder;
}
namespace Stream {

class D3DDevice;
class FrameConverter;
class VideoEncoder;
class LoopbackCapture;

// Capture -> convert -> encode for the stream we send. Lives on its own thread; a timer
// at the stream frame rate encodes whatever the capture produced last, so the output is
// a steady frame rate no matter how often the source repaints.
class StreamSender : public Platform::Trackable
{
public:
    explicit StreamSender(Platform::WorkerLoop &loop);
    ~StreamSender() override;

    void start(const Styx::Core::Stream::CaptureSource &source, const Styx::Core::Stream::StreamQuality &quality,
               bool includeAudio);
    void stop();
    void requestKeyframe();
    void onBandwidthEstimate(unsigned int bitsPerSecond);

    Platform::Signal<int, int, int, int, bool, const std::string &> started;
    Platform::Signal<const std::string &> failed;
    Platform::Signal<> sourceClosed;
    Platform::Signal<const std::string &, int64_t, bool> encodedVideo;
    Platform::Signal<const std::string &, int64_t> encodedAudio;

private:
    void encodeTick();
    bool reopenEncoder(int bitrate);
    bool readBackNv12(ID3D11Texture2D *source, AVFrame *frame);
    void onAudioSamples(const int16_t *samples, int frames);

    std::shared_ptr<D3DDevice> device;
    std::unique_ptr<ScreenCapture> capture;
    std::unique_ptr<FrameConverter> converter;
    std::unique_ptr<VideoEncoder> encoder;
    std::unique_ptr<LoopbackCapture> loopback;
    std::unique_ptr<Audio::OpusEncoder> opus;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> cpuTarget;  // NV12 render target for the CPU path
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cpuStaging; // NV12 readback

    StreamQuality quality;
    Platform::WorkerLoop &loop;
    Platform::TimerId timer = 0;
    std::chrono::steady_clock::time_point clock;
    int64_t frameIndex = 0;
    std::atomic<bool> keyframePending{ true };
    int64_t lastKeyframeMs = -10000;
    uint64_t lastCaptureSequence = 0;
    bool running = false;

    // bitrate adaptation from REMB
    int targetBitrate = 0;
    int estimateBelowCount = 0;
    int estimateAboveCount = 0;

    std::mutex audioMutex;
    std::vector<int16_t> audioPending;
    int64_t audioSamplesSent = 0;
};

} // namespace Stream
} // namespace Core
} // namespace Styx
