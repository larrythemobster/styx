// Ported from ouwou/acheron upstream/video-streams 71b4907
#pragma once

#include "Core/Video/CaptureSource.hpp"
#include "Core/Video/VideoTypes.hpp"
#include "Discord/Voice/VideoReceiver.hpp"
#include "Platform/Image.hpp"
#include "Platform/Signal.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Styx::Core::Video {

struct BroadcastSettings
{
    std::string sourceId;
    // The tallest picture to send; a taller source is scaled down keeping its shape. Zero
    // sends the source at its own size.
    int maxHeight = 720;
    int framerate = 30;
    Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
    // Bits per second.
    int bitrate = 0;
    int maxBitrate = 0;
    // From the server; zero means key frames are made only when asked for.
    int keyframeIntervalMs = 0;
};

struct BroadcastStats
{
    std::string encoderName;
    bool hardware = false;
    FrameSize encodedSize;
    double capturedFps = 0;
    double encodedFps = 0;
    double averageEncodeMs = 0;
    uint64_t bitrateBps = 0;
    uint64_t framesEncoded = 0;
    uint64_t framesDropped = 0;
};

using EncodedFramePointer = std::shared_ptr<const Discord::Voice::EncodedVideoFrame>;

// Captures a source, scales it and encodes it, on a thread of its own. Its signals are
// published from that worker thread, so handlers connecting to them that touch UI or
// thread-unsafe state should dispatch appropriately (e.g. via Platform::Dispatcher).
//
// Encoded frames are whole access units as the RTP sender wants them, with a 90 kHz timestamp
// taken from when the picture was captured: Annex B for H264 and H265, every key frame led by
// its parameter sets; OBUs with their sizes for AV1, every key frame holding a sequence
// header; a bare frame for VP8.
//
// A source whose size the codec's encoder cannot code acceptably (Media::paddingIsAcceptable)
// is captured and reported and not encoded: started() says the size, and nothing follows
// until the pipeline is started again with another codec or the source changes its size.
class BroadcastPipeline
{
public:
    BroadcastPipeline();
    ~BroadcastPipeline();

    BroadcastPipeline(const BroadcastPipeline &) = delete;
    BroadcastPipeline &operator=(const BroadcastPipeline &) = delete;

    // The codecs this machine has a working encoder for, best first. H265 and AV1 are among
    // them only when there is hardware for them.
    // The first call opens each candidate once and can take a moment; call it off the UI thread.
    [[nodiscard]] static std::vector<Discord::Voice::VideoCodec> encodableCodecs();

    // All of these may be called from any thread and return at once.
    void start(const BroadcastSettings &settings);
    void stop();
    // The first frame encoded after this is a key frame.
    void requestKeyframe();
    // Encoders that cannot follow a new rate while running are reopened, which costs a key
    // frame, so callers should move this in steps and not continuously.
    void setBitrate(int bitrate);
    // While nobody is watching: nothing is encoded, and the source is read only as often as
    // the previews need it. Sending again starts with a key frame.
    void setEncodingSuspended(bool suspended);

    // Signals published from the worker thread. Handlers run on the worker thread.
    Platform::Signal<FrameSize, int> started;
    Platform::Signal<EncodedFramePointer> frameEncoded;
    // About twice a second, and small: for the sharer's own tile and for the stream's thumbnail.
    Platform::Signal<Platform::ImageBuffer> previewReady;
    // The source stopped delivering pictures (true) or resumed (false).
    Platform::Signal<bool> captureStalled;
    // The window being shared was closed. Nothing more will come, and nothing went wrong.
    Platform::Signal<> sourceClosed;
    // Nothing more will come: the source is gone, or no encoder could be opened.
    Platform::Signal<std::string> failed;
    Platform::Signal<BroadcastStats> statsUpdated;

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};

} // namespace Styx::Core::Video
