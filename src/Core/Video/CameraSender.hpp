// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#pragma once

#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/BroadcastPipeline.hpp"
#include "Core/Video/CameraCapture.hpp"
#include "Core/Video/CameraPipeline.hpp"
#include "Core/Video/SendQuality.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Platform/Image.hpp"
#include "Platform/Signal.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Styx::Discord::Voice {

struct LocalVideoDescription {
    int maxWidth = 0;
    int maxHeight = 0;
    int maxFramerate = 30;
    int maxBitrate = 0;
    int secondLayerMaxBitrate = 0;
};

struct VideoSendStats {
    std::string codec;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t frames = 0;
    uint64_t keyframes = 0;
    uint64_t retransmitted = 0;
    uint64_t nacksReceived = 0;
    uint64_t keyframeRequests = 0;
    uint64_t bitrateBps = 0;
    double lossFraction = -1.0;
    int roundTripMs = -1;
    int receivers = -1;
    uint64_t receiverEstimateBps = 0;
    uint64_t delayEstimateBps = 0;
    uint64_t acknowledgedBps = 0;
};

} // namespace Styx::Discord::Voice

namespace Styx::Core::Video {

// The account's own camera in the voice channel or call it is connected to. Says on the
// gateway that the camera is on, runs the camera's pipeline, and sends what it encodes on the
// voice connection, at the size, frame rate and bitrate the official client's rules give.
//
// The camera goes off by itself when the account leaves the channel or is moved to another,
// when another of its sessions takes the call over, and when the camera fails.
class CameraSender {
public:
    enum class State {
        Off,
        // The gateway has been told; the camera, its encoder or the voice connection is not
        // up yet.
        Starting,
        On,
        // Off, having gone off by itself. failure() says why.
        Failed,
    };

    struct Stats {
        BroadcastStats encoder;
        Discord::Voice::VideoSendStats network;
        // What the encoder is asked for, in bits per second.
        int targetBitrate = 0;
        // As the server counts them; negative until it has said.
        int receivers = -1;
        // How much of the picture its viewers want, 0 to 100, as the encoder now serves it.
        int servedWant = 100;
        bool mutedFramerate = false;
        bool encodingSuspended = false;

        // The second layer, where the connection has one: the same again for it.
        bool hasSecondLayer = false;
        BroadcastStats secondEncoder;
        int secondTargetBitrate = 0;
        int secondReceivers = -1;
        int secondServedWant = 100;
        bool secondEncodingSuspended = true;
    };

    // Whether the account may turn its camera on where it is in voice now.
    using Permission = std::function<bool()>;

    struct GatewayInterface {
        std::function<void(uint64_t guildId, uint64_t channelId, bool selfMute, bool selfDeaf, bool selfVideo)> sendVoiceState;
    };

    struct VoiceInterface {
        std::function<bool()> isConnected;
        std::function<uint64_t()> currentChannelId;
        std::function<uint64_t()> currentGuildId;
        std::function<bool()> isSelfMuted;
        std::function<bool()> isSelfDeafened;
        std::function<bool()> isSelfVideo;
        std::function<void(const std::vector<Discord::Voice::VideoCodec> &)> setEncodableVideoCodecs;
    };

    struct RtcInterface {
        std::function<void(const Discord::Voice::LocalVideoDescription &)> startVideo;
        std::function<void()> stopVideo;
        std::function<void(int bitrate, int quality)> setVideoBitrate;
        std::function<void(const Discord::Voice::EncodedVideoFrame &frame, int quality)> sendVideoFrame;
    };

    struct ScreenShareInterface {
        std::function<bool()> isSharing;
        std::function<void(std::function<void(const std::vector<Discord::Voice::VideoCodec> &)>)> whenEncodersKnown;
    };

    // What a voice connection offers to send the camera with, of the codecs this machine
    // encodes. Call it once those are known: it asks which encoder each would use.
    [[nodiscard]] static std::vector<Discord::Voice::VideoCodec> codecsToOffer(const std::vector<Discord::Voice::VideoCodec> &encodable);

    explicit CameraSender(Permission allowedHere = [] { return true; });
    ~CameraSender();

    void setGatewayInterface(GatewayInterface gateway);
    void setVoiceInterface(VoiceInterface voice);
    void setRtcInterface(std::shared_ptr<RtcInterface> rtc);
    void setScreenShareInterface(ScreenShareInterface screenShare);

    // A new gateway session has forgotten the camera, so nothing is sent for it.
    void reset();

    [[nodiscard]] bool canTurnOn() const;
    [[nodiscard]] State state() const { return currentState; }
    [[nodiscard]] bool isOn() const { return currentState == State::Starting || currentState == State::On; }
    // What a voice state update for `channelId` has to say of the camera.
    [[nodiscard]] bool isOnIn(uint64_t channelId) const { return isOn() && channelId == cameraChannelId; }
    [[nodiscard]] Stats stats() const;
    // The camera's latest picture, as the camera sees it. Null until there is one.
    [[nodiscard]] Platform::ImageBuffer preview() const { return latestPreview; }
    // Why the camera went off by itself, in one sentence. Empty unless the state is Failed.
    [[nodiscard]] std::string failure() const { return failureText; }
    void clearFailure();

    // An empty id takes the first camera there is. While the camera is on, this changes to
    // another camera without turning it off.
    void turnOn(const std::string &deviceId = "");
    void turnOff();

    // External notifications
    void onRtcConnected();
    void onRtcDisconnected();
    void onRtcClosing();
    void onVoiceDisconnected();
    void onVoiceMembershipChanged();
    void onOwnVoiceStateChanged();
    void onSpeakingChanged(bool nowSpeaking);
    void onScreenShareChanged();
    void onSendCodecChanged(Discord::Voice::VideoCodec codec);
    void onSendStats(const Discord::Voice::VideoSendStats &stats);
    void onSendLayersKnown(const std::vector<int> &qualities);
    void onKeyframeRequested(int quality);
    void onReceiversChanged(int count, int quality);
    void onWantsReceived(int want, int quality);

    // How long the server is given to name a codec the camera can be sent with.
    static constexpr int CODEC_TIMEOUT_MS = 8000;

    // Also sent when the first picture of a camera arrives.
    Platform::Signal<> stateChanged;
    Platform::Signal<const Platform::ImageBuffer &> previewChanged;
    // The camera went off without being asked to. failure() has the same sentence.
    Platform::Signal<const std::string &> failed;

private:
    void onEncodersKnown(const std::vector<Discord::Voice::VideoCodec> &codecs);
    void onPipelineStarted(const FrameSize &encodedSize);
    void onPreviewReady(const Platform::ImageBuffer &preview);
    void onPipelineFailed(const std::string &reason);
    void onPipelineStats(const BroadcastStats &stats, int layer);

    void onCodecTimedOut();
    void applyPlan();

    void setState(State state);
    void end(bool tellGateway, State endState);
    void fail(const std::string &reason, bool tellGateway = true);
    void sendVoiceState();

    void sendOnceReady();
    void startPipeline();
    void stopPipeline();
    void sendFramesToRtc();
    void announceVideo();
    void applySuspension();

    void startPlanTimer(int ms);
    void stopPlanTimer();
    void startCodecTimeout();
    void stopCodecTimeout();
    void timerThreadLoop();

    Permission allowedHere;
    GatewayInterface gateway;
    VoiceInterface voice;
    std::shared_ptr<RtcInterface> rtc;
    ScreenShareInterface screenShare;

    State currentState = State::Off;
    std::string failureText;
    uint64_t cameraChannelId = 0;
    std::string deviceId;
    bool encodersKnown = false;
    uint64_t attempt = 0;

    uint64_t connectionSerial = 0;
    uint64_t pipelineSerial = 0;
    bool rtcConnected = false;
    Discord::Voice::VideoCodec sendCodec = Discord::Voice::VideoCodec::Unknown;
    bool videoAnnounced = false;

    // One encoding of the camera, by its layer in the pipeline: the full picture, and beside
    // it the second layer, which is sent only where the connection has one.
    struct Layer {
        int quality = CAMERA_FULL_QUALITY;
        CameraSendPlan plan;
        CameraEncoding asked;
        // As the server counts them; negative until it has said.
        int receivers = -1;
        bool suspended = false;
        BroadcastStats encoderStats;
    };
    static constexpr int FULL = 0;
    static constexpr int SECOND = 1;
    [[nodiscard]] int layerOf(int quality) const { return quality == layers[SECOND].quality ? SECOND : FULL; }

    std::array<Layer, 2> layers;
    bool hasSecondLayer = false;

    std::unique_ptr<CameraPipeline> pipeline;
    Discord::Voice::VideoCodec pipelineCodec = Discord::Voice::VideoCodec::Unknown;
    bool pipelineStarted = false;

    bool speaking = false;
    std::chrono::steady_clock::time_point clockStart = std::chrono::steady_clock::now();
    [[nodiscard]] int64_t elapsedMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - clockStart).count();
    }

    std::mutex timerMutex;
    std::condition_variable timerCv;
    std::optional<std::chrono::steady_clock::time_point> planTimerDue;
    std::optional<std::chrono::steady_clock::time_point> codecTimerDue;
    bool timerThreadStopping = false;
    std::thread timerThread;

    Platform::ImageBuffer latestPreview;
    Discord::Voice::VideoSendStats networkStats;
};

} // namespace Styx::Core::Video
