#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QSize>
#include <QTimer>

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
class StreamSender : public QObject
{
    Q_OBJECT
public:
    explicit StreamSender(QObject *parent = nullptr);
    ~StreamSender() override;

public slots:
    void start(const Styx::Core::Stream::CaptureSource &source, const Styx::Core::Stream::StreamQuality &quality,
               bool includeAudio);
    void stop();
    void requestKeyframe();
    void onBandwidthEstimate(unsigned int bitsPerSecond);

signals:
    void started(int width, int height, int framerate, int maxBitrate, bool audio, const QString &encoderName);
    void failed(const QString &reason);
    void sourceClosed();
    void encodedVideo(const QByteArray &annexB, qint64 ptsUs, bool keyframe);
    void encodedAudio(const QByteArray &opus, qint64 ptsUs);

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
    QTimer timer{ this }; // parented, so moveToThread() takes it to the capture thread
    QElapsedTimer clock;
    qint64 frameIndex = 0;
    std::atomic<bool> keyframePending{ true };
    qint64 lastKeyframeMs = -10000;
    quint64 lastCaptureSequence = 0;
    bool running = false;

    // bitrate adaptation from REMB
    int targetBitrate = 0;
    int estimateBelowCount = 0;
    int estimateAboveCount = 0;

    std::mutex audioMutex;
    std::vector<int16_t> audioPending;
    qint64 audioSamplesSent = 0;
};

} // namespace Stream
} // namespace Core
} // namespace Styx
