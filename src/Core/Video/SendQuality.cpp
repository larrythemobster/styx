#include "Core/Video/SendQuality.hpp"

#include <algorithm>
#include <cmath>

#include "Platform/Settings.hpp"

namespace Styx::Core::Video {
namespace {
constexpr int NITRO_CLASSIC = 1, NITRO = 2;
constexpr int MIN_BITRATE = 500'000, TARGET_BITRATE = 600'000, STANDARD_MAX_BITRATE = 3'500'000;
constexpr int HIGH_MAX_BITRATE = 9'000'000, BITRATE_FLOOR = 150'000, STANDARD_HEIGHT = 720;
constexpr int STANDARD_FRAMERATE = 30, SLOW_SOURCE_FRAMERATE = 10, PIXEL_FRAMES_PER_BIT = 50;
constexpr int CAMERA_MUTED_FRAMERATE = 20, CAMERA_MUTED_LOWEST_FRAMERATE = 12;
struct CameraRow { int want; int width; int height; };
constexpr std::array<CameraRow, 9> CAMERA_LADDER{{{100,1280,720},{90,896,504},{80,672,378},{70,640,360},{60,512,288},{50,416,234},{40,384,216},{30,320,180},{20,256,144}}};
bool encodeAlike(int want, int other) { return cameraLayerFor(want) == cameraLayerFor(other) && cameraFramerate(want, true) == cameraFramerate(other, true); }
template <typename Values> bool contains(const Values &values, int value) { return std::find(values.begin(), values.end(), value) != values.end(); }
int scaledUp(double share, int bitrate) { return static_cast<int>(std::ceil(share * bitrate)); }
} // namespace

bool mayUseQuality(const SendQuality &quality, const SharerTier &tier)
{
    if (!contains(SEND_HEIGHTS, quality.height) || !contains(SEND_FRAMERATES, quality.framerate)) return false;
    if (quality.isSource()) return tier.premiumType == NITRO;
    const bool paysForIt = tier.premiumType == NITRO_CLASSIC || tier.premiumType == NITRO;
    if (quality.height > STANDARD_HEIGHT) return paysForIt || tier.guildBoostTier >= 2;
    if (quality.framerate > STANDARD_FRAMERATE) return paysForIt || tier.guildBoostTier >= 1;
    return true;
}
SendQuality usableQuality(const SendQuality &wanted, const SharerTier &tier) { return mayUseQuality(wanted, tier) ? wanted : SendQuality{}; }
SendQuality atHeight(const SendQuality &quality, int height, const SharerTier &tier) { SendQuality moved{height, quality.framerate}; if (!mayUseQuality(moved, tier)) moved.framerate = STANDARD_FRAMERATE; return moved; }
FrameSize announcedSize(const SendQuality &quality) { if (quality.isSource()) return {}; return {quality.height == 480 ? quality.height * 4 / 3 : quality.height * 16 / 9, quality.height}; }
BitrateLimits bitrateLimits(const SendQuality &quality)
{
    const bool standard = quality.isSource() ? quality.framerate < SLOW_SOURCE_FRAMERATE : quality.height <= STANDARD_HEIGHT && quality.framerate <= STANDARD_FRAMERATE;
    return {MIN_BITRATE, TARGET_BITRATE, standard ? STANDARD_MAX_BITRATE : HIGH_MAX_BITRATE};
}
BitrateLimits limitsForViewerSize(const BitrateLimits &full, const SendQuality &quality, std::int64_t encodedPixels, int framerate, std::int64_t viewerPixels)
{
    if (quality.isSource() || viewerPixels <= 0 || viewerPixels >= encodedPixels) return full;
    const double share = static_cast<double>(viewerPixels) / encodedPixels;
    const std::int64_t leastMax = std::min<std::int64_t>(encodedPixels * framerate / PIXEL_FRAMES_PER_BIT, full.max);
    return {std::max(scaledUp(share, full.min), BITRATE_FLOOR), std::max(scaledUp(share, full.target), BITRATE_FLOOR), std::max(scaledUp(share, full.max), static_cast<int>(leastMax))};
}
bool ViewerSize::offer(std::int64_t pixelCount) { const std::int64_t difference = pixelCount > adopted ? pixelCount - adopted : adopted - pixelCount; if (difference * 20 <= adopted) return false; adopted = pixelCount; return true; }
std::int64_t estimatedCeiling(std::uint64_t receiverEstimate, std::uint64_t delayEstimate)
{
    if (!Platform::Settings::instance().getBool("video/delay_estimate", true)) delayEstimate = 0;
    return static_cast<std::int64_t>(receiverEstimate == 0 || delayEstimate == 0 ? std::max(receiverEstimate, delayEstimate) : std::min(receiverEstimate, delayEstimate));
}
bool cameraSecondLayerEnabled() { return Platform::Settings::instance().getBool("video/camera_second_layer", false); }
void setCameraSecondLayerEnabled(bool enabled) { Platform::Settings::instance().setBool("video/camera_second_layer", enabled); }
int cameraMaxBitrate(std::int64_t pixels) { const std::int64_t fullPixels = std::int64_t(CAMERA_CAPTURE_SIZE.width) * CAMERA_CAPTURE_SIZE.height; return static_cast<int>(std::max<std::int64_t>(CAMERA_MAX_BITRATE * pixels / fullPixels, CAMERA_MIN_BITRATE)); }
CameraLayer cameraLayerFor(int want) { const auto row = std::find_if(CAMERA_LADDER.begin(), CAMERA_LADDER.end(), [want](const CameraRow &candidate) { return candidate.want <= want; }); const CameraRow &chosen = row != CAMERA_LADDER.end() ? *row : CAMERA_LADDER.back(); return {chosen.want, {chosen.width, chosen.height}, cameraMaxBitrate(std::int64_t(chosen.width) * chosen.height)}; }
int cameraFramerate(int want, bool muted) { if (!muted) return CAMERA_FRAMERATE; return want <= CAMERA_LADDER.back().want ? CAMERA_MUTED_LOWEST_FRAMERATE : CAMERA_MUTED_FRAMERATE; }
void CameraSendPlan::start(std::int64_t nowMs) { want = 100; wantInUse = want; rate.restart(CAMERA_MIN_BITRATE, maxBitrate()); bitrateInUse = rate.bitrate(); changedAtMs = nowMs; silentSinceMs = nowMs; }
void CameraSendPlan::setWant(int wanted) { want = wanted; if (encodeAlike(want, wantInUse)) wantInUse = want; }
void CameraSendPlan::setSelfMuted(bool muted) { selfMuted = muted; }
void CameraSendPlan::setSpeaking(bool nowSpeaking, std::int64_t nowMs) { if (speaking && !nowSpeaking) silentSinceMs = nowMs; speaking = nowSpeaking; }
void CameraSendPlan::setSharingScreen(bool sharing) { sharingScreen = sharing; }
void CameraSendPlan::reportLoss(double lossFraction, std::int64_t nowMs) { rate.report(lossFraction, nowMs); }
void CameraSendPlan::setReceiverLimit(std::int64_t bitsPerSecond) { rate.setReceiverLimit(bitsPerSecond); }
bool CameraSendPlan::countsAsMuted(std::int64_t nowMs) const { return !sharingScreen && (selfMuted || (!speaking && nowMs - silentSinceMs >= SILENCE_BEFORE_MUTED_MS)); }
bool CameraSendPlan::changeIsWaiting() const { return wantInUse != want || bitrateInUse != rate.bitrate(); }
CameraEncoding CameraSendPlan::advance(std::int64_t nowMs) { if (changeIsWaiting() && nowMs - changedAtMs >= SETTLE_MS) { if (wantInUse != want) { wantInUse = want; rate.setLimits(CAMERA_MIN_BITRATE, maxBitrate()); } bitrateInUse = rate.bitrate(); changedAtMs = nowMs; } return {cameraLayerFor(wantInUse).size, cameraFramerate(wantInUse, countsAsMuted(nowMs)), bitrateInUse}; }
std::optional<std::int64_t> CameraSendPlan::nextChangeIn(std::int64_t nowMs) const { std::optional<std::int64_t> soonest; if (changeIsWaiting()) soonest = std::max<std::int64_t>(changedAtMs + SETTLE_MS - nowMs, 0); const bool silenceIsCounting = !sharingScreen && !selfMuted && !speaking && !countsAsMuted(nowMs); if (silenceIsCounting) { const auto untilMuted = silentSinceMs + SILENCE_BEFORE_MUTED_MS - nowMs; soonest = soonest ? std::min(*soonest, untilMuted) : untilMuted; } return soonest; }
} // namespace Styx::Core::Video
