// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#include "Core/Video/CameraSender.hpp"

#include <algorithm>
#include <utility>

namespace Styx::Core::Video {

using Discord::Voice::LocalVideoDescription;
using Discord::Voice::VideoCodec;
using Discord::Voice::VideoSendStats;

CameraSender::CameraSender(Permission allowedHere)
    : allowedHere(std::move(allowedHere)) {
    layers[SECOND].quality = CAMERA_SECOND_QUALITY;
    layers[SECOND].suspended = true;

    timerThread = std::thread([this] { timerThreadLoop(); });
}

CameraSender::~CameraSender() {
    {
        std::lock_guard lock(timerMutex);
        timerThreadStopping = true;
    }
    timerCv.notify_all();
    if (timerThread.joinable())
        timerThread.join();

    stopPipeline();
}

std::vector<VideoCodec> CameraSender::codecsToOffer(const std::vector<VideoCodec> &encodable) {
    return Media::cameraCodecs(encodable, Media::videoEncoderFor(VideoCodec::AV1));
}

void CameraSender::setGatewayInterface(GatewayInterface g) {
    gateway = std::move(g);
}

void CameraSender::setVoiceInterface(VoiceInterface v) {
    voice = std::move(v);
}

void CameraSender::setRtcInterface(std::shared_ptr<RtcInterface> r) {
    rtc = std::move(r);
    sendFramesToRtc();
}

void CameraSender::setScreenShareInterface(ScreenShareInterface s) {
    screenShare = std::move(s);
}

bool CameraSender::canTurnOn() const {
    const bool voiceOk = voice.isConnected ? voice.isConnected() : false;
    return cameraCaptureAvailable() && voiceOk && allowedHere();
}

void CameraSender::reset() {
    if (isOn())
        fail("Your camera was turned off because the connection to Discord was restarted.", false);
}

void CameraSender::setState(State state) {
    if (currentState == state)
        return;
    currentState = state;
    stateChanged.publish();
}

void CameraSender::clearFailure() {
    if (currentState != State::Failed)
        return;
    failureText.clear();
    setState(State::Off);
}

CameraSender::Stats CameraSender::stats() const {
    const Layer &full = layers[FULL];
    const Layer &second = layers[SECOND];

    Stats now;
    now.encoder = full.encoderStats;
    now.network = networkStats;
    now.targetBitrate = full.asked.bitrate;
    now.receivers = full.receivers;
    now.servedWant = full.plan.servedWant();
    now.mutedFramerate = pipeline && full.plan.countsAsMuted(elapsedMs());
    now.encodingSuspended = full.suspended;
    now.hasSecondLayer = hasSecondLayer;
    now.secondEncoder = second.encoderStats;
    now.secondTargetBitrate = second.asked.bitrate;
    now.secondReceivers = second.receivers;
    now.secondServedWant = second.plan.servedWant();
    now.secondEncodingSuspended = second.suspended;
    return now;
}

void CameraSender::turnOn(const std::string &wantedDeviceId) {
    if (isOn()) {
        if (wantedDeviceId == deviceId)
            return;
        deviceId = wantedDeviceId;
        if (pipeline)
            startPipeline();
        return;
    }

    failureText.clear();
    if (!canTurnOn()) {
        fail("You cannot turn on your camera here.");
        return;
    }

    deviceId = wantedDeviceId;
    cameraChannelId = voice.currentChannelId ? voice.currentChannelId() : 0;
    const int64_t now = elapsedMs();
    for (Layer &layer : layers) {
        layer.plan.start(now);
        layer.plan.setSelfMuted(voice.isSelfMuted ? voice.isSelfMuted() : false);
        layer.plan.setSpeaking(speaking, now);
        layer.plan.setSharingScreen(screenShare.isSharing ? screenShare.isSharing() : false);
    }
    setState(State::Starting);
    sendVoiceState();

    if (screenShare.whenEncodersKnown) {
        screenShare.whenEncodersKnown([this, thisAttempt = ++attempt](const std::vector<VideoCodec> &codecs) {
            if (thisAttempt == attempt)
                onEncodersKnown(codecs);
        });
    } else {
        const auto encodable = Media::encodableVideoCodecs();
        onEncodersKnown(Media::allowedSendCodecs(encodable));
    }
}

void CameraSender::onEncodersKnown(const std::vector<VideoCodec> &codecs) {
    if (codecs.empty()) {
        fail("This machine has no video encoder that Discord accepts, so it cannot send a camera.");
        return;
    }

    encodersKnown = true;
    if (voice.setEncodableVideoCodecs)
        voice.setEncodableVideoCodecs(codecsToOffer(codecs));
    sendOnceReady();
}

void CameraSender::turnOff() {
    end(true, State::Off);
}

void CameraSender::end(bool tellGateway, State endState) {
    if (!isOn())
        return;

    attempt++;
    encodersKnown = false;
    stopCodecTimeout();
    stopPlanTimer();

    if (rtc && rtc->stopVideo)
        rtc->stopVideo();
    videoAnnounced = false;
    stopPipeline();
    latestPreview = {};
    networkStats = {};

    const uint64_t left = std::exchange(cameraChannelId, 0);
    currentState = endState;

    const uint64_t here = voice.currentChannelId ? voice.currentChannelId() : 0;
    const bool selfVideo = voice.isSelfVideo ? voice.isSelfVideo() : false;
    if (tellGateway && here != 0 && (here == left || selfVideo))
        sendVoiceState();

    stateChanged.publish();
}

void CameraSender::fail(const std::string &reason, bool tellGateway) {
    failureText = reason;
    if (isOn())
        end(tellGateway, State::Failed);
    else
        setState(State::Failed);
    failed.publish(reason);
}

void CameraSender::sendVoiceState() {
    if (!gateway.sendVoiceState || !voice.currentChannelId)
        return;
    const uint64_t channelId = voice.currentChannelId();
    const uint64_t guildId = voice.currentGuildId ? voice.currentGuildId() : 0;
    const bool selfMute = voice.isSelfMuted ? voice.isSelfMuted() : false;
    const bool selfDeaf = voice.isSelfDeafened ? voice.isSelfDeafened() : false;
    gateway.sendVoiceState(guildId, channelId, selfMute, selfDeaf, isOnIn(channelId));
}

void CameraSender::onCodecTimedOut() {
    fail("Your camera could not be turned on: Discord named no video codec this machine can encode.");
}

void CameraSender::onVoiceDisconnected() {
    speaking = false;
    end(false, State::Off);
    clearFailure();
}

void CameraSender::onVoiceMembershipChanged() {
    const uint64_t currentChannel = voice.currentChannelId ? voice.currentChannelId() : 0;
    if (isOn() && currentChannel != cameraChannelId)
        end(true, State::Off);
}

void CameraSender::onOwnVoiceStateChanged() {
    const bool selfMute = voice.isSelfMuted ? voice.isSelfMuted() : false;
    for (Layer &layer : layers)
        layer.plan.setSelfMuted(selfMute);
    applyPlan();
}

void CameraSender::onSpeakingChanged(bool nowSpeaking) {
    speaking = nowSpeaking;
    const int64_t now = elapsedMs();
    for (Layer &layer : layers)
        layer.plan.setSpeaking(nowSpeaking, now);
    applyPlan();
}

void CameraSender::onScreenShareChanged() {
    const bool sharing = screenShare.isSharing ? screenShare.isSharing() : false;
    for (Layer &layer : layers)
        layer.plan.setSharingScreen(sharing);
    applyPlan();
}

void CameraSender::onRtcConnected() {
    rtcConnected = true;
    videoAnnounced = false;
    sendOnceReady();
}

void CameraSender::onRtcDisconnected() {
    rtcConnected = false;
    stopCodecTimeout();
    if (currentState == State::On)
        setState(State::Starting);
    applySuspension();
}

void CameraSender::onRtcClosing() {
    connectionSerial++;
    rtc = nullptr;
    rtcConnected = false;
    videoAnnounced = false;
    sendCodec = VideoCodec::Unknown;
    hasSecondLayer = false;
    for (Layer &layer : layers)
        layer.receivers = -1;
    stopCodecTimeout();

    if (currentState == State::On)
        setState(State::Starting);
    applySuspension();
}

void CameraSender::onSendCodecChanged(VideoCodec codec) {
    sendCodec = codec;
    sendOnceReady();
}

void CameraSender::onSendStats(const VideoSendStats &sent) {
    networkStats = sent;
    if (!pipelineStarted)
        return;

    Layer &full = layers[FULL];
    Layer &second = layers[SECOND];
    const int64_t ceiling = estimatedCeiling(sent.receiverEstimateBps, sent.delayEstimateBps);
    const bool sharedWithSecond = ceiling > 0 && hasSecondLayer && !second.suspended;
    second.plan.setReceiverLimit(ceiling);
    full.plan.setReceiverLimit(sharedWithSecond ? std::max<int64_t>(ceiling - second.asked.bitrate, 1) : ceiling);
    const int64_t now = elapsedMs();
    for (Layer &layer : layers)
        layer.plan.reportLoss(sent.lossFraction, now);
    applyPlan();
}

void CameraSender::onSendLayersKnown(const std::vector<int> &qualities) {
    hasSecondLayer = std::find(qualities.begin(), qualities.end(), layers[SECOND].quality) != qualities.end();
    applySuspension();
}

void CameraSender::onKeyframeRequested(int quality) {
    if (pipeline)
        pipeline->requestKeyframe(layerOf(quality));
}

void CameraSender::onReceiversChanged(int count, int quality) {
    layers[static_cast<size_t>(layerOf(quality))].receivers = count;
    applySuspension();
}

void CameraSender::onWantsReceived(int want, int quality) {
    layers[static_cast<size_t>(layerOf(quality))].plan.setWant(want);
    applyPlan();
}

void CameraSender::sendOnceReady() {
    if (!isOn() || !encodersKnown || !rtcConnected)
        return;

    if (sendCodec == VideoCodec::Unknown) {
        startCodecTimeout();
        return;
    }
    stopCodecTimeout();

    if (!pipeline || pipelineCodec != sendCodec)
        startPipeline();
    else if (pipelineStarted && !videoAnnounced)
        announceVideo();
}

void CameraSender::startPipeline() {
    stopPipeline();

    pipeline = std::make_unique<CameraPipeline>();
    pipelineCodec = sendCodec;
    const uint64_t serial = ++pipelineSerial;

    pipeline->started.connect([this, serial](const FrameSize &size, int, int layer) {
        if (serial == pipelineSerial && layer == FULL)
            onPipelineStarted(size);
    });
    pipeline->previewReady.connect([this, serial](const Platform::ImageBuffer &preview) {
        if (serial == pipelineSerial)
            onPreviewReady(preview);
    });
    pipeline->failed.connect([this, serial](const std::string &reason) {
        if (serial == pipelineSerial)
            onPipelineFailed(reason);
    });
    pipeline->statsUpdated.connect([this, serial](const BroadcastStats &encoder, int layer) {
        if (serial == pipelineSerial)
            onPipelineStats(encoder, layer);
    });
    sendFramesToRtc();

    const int64_t now = elapsedMs();
    for (Layer &layer : layers)
        layer.asked = layer.plan.advance(now);
    const Layer &full = layers[FULL];
    const Layer &second = layers[SECOND];
    CameraSettings settings;
    settings.deviceId = deviceId;
    settings.codec = sendCodec;
    settings.encodeSize = full.asked.size;
    settings.framerate = full.asked.framerate;
    settings.bitrate = full.asked.bitrate;
    settings.maxBitrate = CAMERA_MAX_BITRATE;

    pipeline->setEncoding(second.asked.size, second.asked.framerate, SECOND);
    pipeline->setBitrate(second.asked.bitrate, SECOND);
    pipeline->start(settings);
    applyPlan();
}

void CameraSender::stopPipeline() {
    if (!pipeline)
        return;

    pipelineSerial++;
    stopPlanTimer();
    pipeline->stop();
    pipeline.reset();

    pipelineStarted = false;
    for (Layer &layer : layers) {
        layer.suspended = (&layer == &layers[SECOND]);
        layer.encoderStats = {};
    }
}

void CameraSender::sendFramesToRtc() {
    if (!pipeline || !rtc)
        return;

    pipeline->frameEncoded.connect([this, secondQuality = layers[SECOND].quality](const EncodedFramePointer &frame, int layer) {
        if (rtc && rtc->sendVideoFrame && frame) {
            rtc->sendVideoFrame(*frame, layer == SECOND ? secondQuality : CAMERA_FULL_QUALITY);
        }
    });
}

void CameraSender::onPipelineStarted(const FrameSize &encodedSize) {
    (void)encodedSize;
    pipelineStarted = true;
    sendOnceReady();
    applySuspension();
}

void CameraSender::onPreviewReady(const Platform::ImageBuffer &preview) {
    const bool first = latestPreview.empty();
    latestPreview = preview;
    previewChanged.publish(preview);
    if (first)
        stateChanged.publish();
}

void CameraSender::onPipelineFailed(const std::string &reason) {
    fail("Your camera was turned off: " + reason);
}

void CameraSender::onPipelineStats(const BroadcastStats &encoder, int layer) {
    if (layer == FULL || layer == SECOND)
        layers[static_cast<size_t>(layer)].encoderStats = encoder;
}

void CameraSender::announceVideo() {
    LocalVideoDescription description;
    description.maxWidth = CAMERA_CAPTURE_SIZE.width;
    description.maxHeight = CAMERA_CAPTURE_SIZE.height;
    description.maxFramerate = CAMERA_FRAMERATE;
    description.maxBitrate = CAMERA_MAX_BITRATE;
    description.secondLayerMaxBitrate = layers[SECOND].plan.maxBitrate();

    videoAnnounced = true;
    if (rtc) {
        if (rtc->startVideo)
            rtc->startVideo(description);
        if (rtc->setVideoBitrate) {
            rtc->setVideoBitrate(layers[FULL].asked.bitrate, CAMERA_FULL_QUALITY);
            rtc->setVideoBitrate(layers[SECOND].asked.bitrate, layers[SECOND].quality);
        }
    }
    applySuspension();
    pipeline->requestKeyframe(FULL);
    pipeline->requestKeyframe(SECOND);
    setState(State::On);
}

void CameraSender::applyPlan() {
    stopPlanTimer();
    if (!pipeline)
        return;

    const int64_t now = elapsedMs();
    std::optional<int64_t> nextChange;
    for (int index = 0; index < static_cast<int>(layers.size()); index++) {
        Layer &layer = layers[static_cast<size_t>(index)];
        const CameraEncoding wanted = layer.plan.advance(now);
        if (wanted.bitrate != layer.asked.bitrate) {
            pipeline->setBitrate(wanted.bitrate, index);
            if (rtc && rtc->setVideoBitrate)
                rtc->setVideoBitrate(wanted.bitrate, layer.quality);
        }
        if (wanted.size != layer.asked.size || wanted.framerate != layer.asked.framerate)
            pipeline->setEncoding(wanted.size, wanted.framerate, index);
        layer.asked = wanted;

        if (const std::optional<int64_t> wait = layer.plan.nextChangeIn(now))
            nextChange = nextChange ? std::min(*nextChange, *wait) : *wait;
    }
    if (nextChange)
        startPlanTimer(static_cast<int>(*nextChange));
}

void CameraSender::applySuspension() {
    if (!pipeline)
        return;

    Layer &full = layers[FULL];
    Layer &second = layers[SECOND];
    const bool suspendFull = pipelineStarted && (!videoAnnounced || full.receivers == 0);
    const bool suspendSecond = !hasSecondLayer || !pipelineStarted || !videoAnnounced || second.receivers == 0;
    if (suspendFull != std::exchange(full.suspended, suspendFull))
        pipeline->setEncodingSuspended(suspendFull, FULL);
    if (suspendSecond != std::exchange(second.suspended, suspendSecond))
        pipeline->setEncodingSuspended(suspendSecond, SECOND);
}

void CameraSender::startPlanTimer(int ms) {
    std::lock_guard lock(timerMutex);
    planTimerDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    timerCv.notify_all();
}

void CameraSender::stopPlanTimer() {
    std::lock_guard lock(timerMutex);
    planTimerDue.reset();
    timerCv.notify_all();
}

void CameraSender::startCodecTimeout() {
    std::lock_guard lock(timerMutex);
    if (!codecTimerDue) {
        codecTimerDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(CODEC_TIMEOUT_MS);
        timerCv.notify_all();
    }
}

void CameraSender::stopCodecTimeout() {
    std::lock_guard lock(timerMutex);
    codecTimerDue.reset();
    timerCv.notify_all();
}

void CameraSender::timerThreadLoop() {
    std::unique_lock lock(timerMutex);
    while (!timerThreadStopping) {
        if (!planTimerDue && !codecTimerDue) {
            timerCv.wait(lock, [this] { return timerThreadStopping || planTimerDue || codecTimerDue; });
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        auto wakeAt = std::chrono::steady_clock::time_point::max();
        if (planTimerDue) wakeAt = std::min(wakeAt, *planTimerDue);
        if (codecTimerDue) wakeAt = std::min(wakeAt, *codecTimerDue);

        if (wakeAt <= now) {
            bool firePlan = false;
            bool fireCodec = false;
            if (planTimerDue && *planTimerDue <= now) {
                planTimerDue.reset();
                firePlan = true;
            }
            if (codecTimerDue && *codecTimerDue <= now) {
                codecTimerDue.reset();
                fireCodec = true;
            }
            lock.unlock();
            if (firePlan)
                applyPlan();
            if (fireCodec)
                onCodecTimedOut();
            lock.lock();
            continue;
        }

        timerCv.wait_until(lock, wakeAt, [this, wakeAt] {
            if (timerThreadStopping) return true;
            auto now = std::chrono::steady_clock::now();
            if (planTimerDue && *planTimerDue <= now) return true;
            if (codecTimerDue && *codecTimerDue <= now) return true;
            return false;
        });
    }
}

} // namespace Styx::Core::Video
