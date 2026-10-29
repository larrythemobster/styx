#pragma once

#ifdef STYX_HAVE_FFMPEG

#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include "Platform/Signal.hpp"
#include "Platform/Trackable.hpp"
#include <atomic>
#include <cstdint>
#include <string>

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
class StreamDecoder : public Platform::Trackable
{
public:
    StreamDecoder() = default;
    ~StreamDecoder();

    void setTargetSize(const Platform::Size &size);
    void decode(const std::string &annexB, uint32_t rtpTimestamp, bool keyframe);

    Platform::Signal<const Platform::ImageBuffer &, const Platform::Size &> frameDecoded;
    Platform::Signal<> needKeyframe;

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
