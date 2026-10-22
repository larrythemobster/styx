#pragma once

#if defined(_WIN32) && defined(STYX_HAVE_FFMPEG)

#include <QByteArray>
#include <QSize>
#include <QString>

#include <d3d11.h>

#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

struct SwsContext;

namespace Styx {
namespace Core {
namespace Stream {

class D3DDevice;

struct EncoderSettings
{
    QSize size;
    int framerate = 30;
    int bitrate = 4'000'000;
    int maxBitrate = 6'000'000;
};

struct EncodedPacket
{
    QByteArray data; // Annex B access unit; keyframes always carry SPS/PPS
    int64_t ptsUs = 0;
    bool keyframe = false;
};

// H264 for Go Live through FFmpeg. Hardware encoders that take D3D11 surfaces (AMF, NVENC)
// are fed straight from the capture GPU; the rest (QSV, Media Foundation, OpenH264) get an
// NV12 copy read back from the GPU. Tuned for screen content at low latency: no B-frames,
// CBR, an IDR every few seconds and on demand, parameter sets on every IDR.
class VideoEncoder
{
public:
    explicit VideoEncoder(std::shared_ptr<D3DDevice> device);
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;

    bool open(const EncoderSettings &settings, QString *error);
    void close();

    [[nodiscard]] const QString &name() const { return encoderName; }
    [[nodiscard]] bool takesGpuSurfaces() const { return gpuInput; }
    [[nodiscard]] const EncoderSettings &settings() const { return current; }

    // GPU path: a pooled NV12 surface to render the next frame into.
    AVFrame *acquireGpuFrame(ID3D11Texture2D **texture, UINT *arrayIndex);
    // CPU path: an NV12 frame to fill from a readback.
    AVFrame *acquireCpuFrame();

    // Takes ownership of frame. Appends any packets the encoder finished.
    bool encode(AVFrame *frame, int64_t ptsUs, bool forceKeyframe, std::vector<EncodedPacket> &out);

private:
    bool tryOpen(const char *name, bool gpu, QString *error);
    bool drain(std::vector<EncodedPacket> &out);

    std::shared_ptr<D3DDevice> device;
    EncoderSettings current;
    QString encoderName;
    bool gpuInput = false;

    AVCodecContext *ctx = nullptr;
    AVBufferRef *hwDevice = nullptr;
    AVBufferRef *hwFrames = nullptr;
    AVPixelFormat cpuFormat = AV_PIX_FMT_NV12;
    SwsContext *sws = nullptr;
    QByteArray parameterSets; // SPS+PPS from the latest keyframe, for keyframes that lack them
};

} // namespace Stream
} // namespace Core
} // namespace Styx

#endif
