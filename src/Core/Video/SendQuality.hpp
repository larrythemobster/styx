#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "Core/Video/SendRateController.hpp"
#include "Core/Video/VideoTypes.hpp"

namespace Styx::Core::Video {

// What a screen share is sent at, as its owner chose it. The rules are ported from
// ouwou/Acheron's video-streams implementation.
struct SendQuality {
    static constexpr int SOURCE_HEIGHT = 0;
    int height = 720;
    int framerate = 30;
    [[nodiscard]] bool isSource() const { return height == SOURCE_HEIGHT; }
    bool operator==(const SendQuality &) const = default;
};

inline constexpr std::array<int, 5> SEND_HEIGHTS{480, 720, 1080, 1440, SendQuality::SOURCE_HEIGHT};
inline constexpr std::array<int, 3> SEND_FRAMERATES{15, 30, 60};

struct SharerTier { int premiumType = 0; int guildBoostTier = 0; };
[[nodiscard]] bool mayUseQuality(const SendQuality &quality, const SharerTier &tier);
[[nodiscard]] SendQuality usableQuality(const SendQuality &wanted, const SharerTier &tier);
[[nodiscard]] SendQuality atHeight(const SendQuality &quality, int height, const SharerTier &tier);
[[nodiscard]] FrameSize announcedSize(const SendQuality &quality);

struct BitrateLimits {
    int min = 0;
    int target = 0;
    int max = 0;
    bool operator==(const BitrateLimits &) const = default;
};
[[nodiscard]] BitrateLimits bitrateLimits(const SendQuality &quality);
[[nodiscard]] BitrateLimits limitsForViewerSize(const BitrateLimits &full, const SendQuality &quality,
    std::int64_t encodedPixels, int framerate, std::int64_t viewerPixels);

class ViewerSize {
public:
    bool offer(std::int64_t pixelCount);
    [[nodiscard]] std::int64_t pixelCount() const { return adopted; }
private:
    std::int64_t adopted = 0;
};

[[nodiscard]] std::int64_t estimatedCeiling(std::uint64_t receiverEstimate, std::uint64_t delayEstimate);

inline constexpr FrameSize CAMERA_CAPTURE_SIZE{1280, 720};
inline constexpr int CAMERA_FRAMERATE = 30;
inline constexpr int CAMERA_MIN_BITRATE = 150'000;
inline constexpr int CAMERA_MAX_BITRATE = 2'500'000;
[[nodiscard]] bool cameraSecondLayerEnabled();
void setCameraSecondLayerEnabled(bool enabled);
inline constexpr int CAMERA_FULL_QUALITY = 100;
inline constexpr int CAMERA_SECOND_QUALITY = 50;

struct CameraLayer {
    int want = 0;
    FrameSize size;
    int maxBitrate = 0;
    bool operator==(const CameraLayer &) const = default;
};
[[nodiscard]] int cameraMaxBitrate(std::int64_t pixels);
[[nodiscard]] CameraLayer cameraLayerFor(int want);
[[nodiscard]] int cameraFramerate(int want, bool muted);

struct CameraEncoding {
    FrameSize size;
    int framerate = 0;
    int bitrate = 0;
    bool operator==(const CameraEncoding &) const = default;
};

class CameraSendPlan {
public:
    void start(std::int64_t nowMs);
    void setWant(int wanted);
    void setSelfMuted(bool muted);
    void setSpeaking(bool nowSpeaking, std::int64_t nowMs);
    void setSharingScreen(bool sharing);
    void reportLoss(double lossFraction, std::int64_t nowMs);
    void setReceiverLimit(std::int64_t bitsPerSecond);
    CameraEncoding advance(std::int64_t nowMs);
    [[nodiscard]] std::optional<std::int64_t> nextChangeIn(std::int64_t nowMs) const;
    [[nodiscard]] int servedWant() const { return wantInUse; }
    [[nodiscard]] int maxBitrate() const { return cameraLayerFor(wantInUse).maxBitrate; }
    [[nodiscard]] bool countsAsMuted(std::int64_t nowMs) const;
    static constexpr std::int64_t SETTLE_MS = 2000;
    static constexpr std::int64_t SILENCE_BEFORE_MUTED_MS = 15000;
private:
    [[nodiscard]] bool changeIsWaiting() const;
    int want = 100;
    int wantInUse = 100;
    bool selfMuted = false;
    bool speaking = false;
    bool sharingScreen = false;
    std::int64_t silentSinceMs = 0;
    SendRateController rate;
    int bitrateInUse = 0;
    std::int64_t changedAtMs = 0;
};

} // namespace Styx::Core::Video
