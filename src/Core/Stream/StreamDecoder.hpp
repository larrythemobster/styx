#pragma once

#ifdef STYX_HAVE_FFMPEG

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>

#include <atomic>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct SwsContext;

namespace Styx {
namespace Core {
namespace Stream {

// H264 access units from a watched stream -> RGB images at the size the view shows them.
// Decodes on the GPU (D3D11VA) when it can, in software otherwise. Lives on its own thread.
class StreamDecoder : public QObject
{
    Q_OBJECT
public:
    explicit StreamDecoder(QObject *parent = nullptr);
    ~StreamDecoder() override;

    // Thread-safe; the next frame is scaled to fit this (device pixels).
    void setTargetSize(const QSize &size);

public slots:
    void decode(const QByteArray &annexB, quint32 rtpTimestamp, bool keyframe);

signals:
    void frameDecoded(const QImage &image, const QSize &sourceSize);
    // The decoder lost its reference picture; ask the streamer for a keyframe.
    void needKeyframe();

private:
    bool openDecoder(bool hardware);
    void close();
    void present(AVFrame *frame);

    AVCodecContext *ctx = nullptr;
    AVBufferRef *hwDevice = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *swFrame = nullptr;
    SwsContext *sws = nullptr;
    bool hardware = false;
    bool hardwareFailed = false;
    bool haveKeyframe = false;
    int consecutiveErrors = 0;

    std::atomic<int> targetWidth{ 1280 };
    std::atomic<int> targetHeight{ 720 };
};

} // namespace Stream
} // namespace Core
} // namespace Styx

#endif
