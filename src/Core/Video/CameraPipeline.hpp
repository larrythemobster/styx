// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#pragma once

#include "Core/Video/BroadcastPipeline.hpp"
#include "Core/Video/CameraCapture.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Platform/Image.hpp"
#include "Platform/Signal.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Styx::Core::Video {

// The camera the user chose last (`video/camera_device`), when it is among `attached`. Empty
// otherwise, which takes the first there is.
[[nodiscard]] std::string preferredCamera(const std::vector<CameraDevice> &attached);
void setPreferredCamera(const std::string &deviceId);

struct CameraSettings {
    // Empty takes the first camera there is.
    std::string deviceId;
    Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
    // What is encoded. The camera itself is always asked for 1280x720 at 30, or the nearest it
    // has, and its picture is scaled and its frames thinned to this.
    FrameSize encodeSize{1280, 720};
    int framerate = 30;
    // Bits per second.
    int bitrate = 0;
    int maxBitrate = 0;
    // From the server; zero means key frames are made only when asked for.
    int keyframeIntervalMs = 0;
};

// Opens a camera and encodes its picture, on a thread of its own, with the same manners as
// BroadcastPipeline: its signals are published from that thread, and encoded frames are whole
// access units with a 90 kHz timestamp, in the shapes described there.
class CameraPipeline {
public:
    CameraPipeline();
    ~CameraPipeline();

    // The camera's pictures can be encoded more than once, each layer at a size, frame rate
    // and bitrate of its own with an encoder of its own. Layer 0 is the one the settings
    // describe. The others encode nothing until they are taken out of suspension, and start
    // as layer 0 does unless they are told otherwise first.
    static constexpr int LAYERS = 2;

    // All of these may be called from any thread and return at once.
    void start(const CameraSettings &settings);
    void stop();
    // The first frame the layer encodes after this is a key frame.
    void requestKeyframe(int layer = 0);
    // Encoders that cannot follow a new rate while running are reopened, which costs a key
    // frame, so callers should move this in steps and not continuously.
    void setBitrate(int bitrate, int layer = 0);
    // A new size or frame rate to encode at, the camera staying open. Starts with a key frame.
    void setEncoding(const FrameSize &encodeSize, int framerate, int layer = 0);
    // While nobody is watching: the camera stays open, so the preview stays live, and the
    // layer encodes nothing. Sending again starts with a key frame.
    void setEncodingSuspended(bool suspended, int layer = 0);

    Platform::Signal<const FrameSize &, int, int> started;
    Platform::Signal<const EncodedFramePointer &, int> frameEncoded;
    // The camera's own picture for its owner to look at: no larger than 640x360, about fifteen
    // times a second, not mirrored.
    Platform::Signal<const Platform::ImageBuffer &> previewReady;
    // Nothing more will come: the camera is gone or in use elsewhere, or no encoder opened.
    Platform::Signal<const std::string &> failed;
    Platform::Signal<const BroadcastStats &, int> statsUpdated;

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};

} // namespace Styx::Core::Video
