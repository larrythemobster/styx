// Behavioral regression tests ported from ouwou/Acheron's tst_SendQuality,
// tst_DecodeDemand, and tst_SinkWants video-streams tests.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <optional>

#include "Core/Media/DecodeDemand.hpp"
#include "Core/Video/SendQuality.hpp"
#include "Core/Video/TileVisibility.hpp"
#include "Discord/Voice/SinkWants.hpp"

using namespace Styx;
using namespace Styx::Core::Video;
using Styx::Core::Media::DecodeDemand;
using Styx::Core::TileVisibility;
using namespace Styx::Discord::Voice;

namespace {
VideoSender camera(std::uint64_t user, std::uint32_t ssrc, bool twoLayers = true)
{
    VideoSender sender{.userId = user};
    sender.layers.push_back({ssrc, ssrc + 1, 100});
    if (twoLayers) sender.layers.push_back({ssrc + 2, ssrc + 3, 50});
    return sender;
}
bool observeFor(DecodeDemand &demand, std::int64_t &now, int seconds, int backlog, std::uint64_t discards = 0)
{
    bool changed = false;
    for (int second = 0; second < seconds; ++second) { now += 1000; changed = demand.observe(backlog, discards, now) || changed; }
    return changed;
}
CameraSendPlan speakingPlan() { CameraSendPlan plan; plan.start(0); plan.setSpeaking(true, 0); return plan; }
} // namespace

TEST_CASE("Video quality entitlement, announcement, and bitrate ladders", "[video-adaptation]")
{
    for (int height : {480, 720}) for (int fps : {15, 30}) REQUIRE(mayUseQuality({height, fps}, {}));
    REQUIRE(mayUseQuality({720, 60}, {0, 1}));
    REQUIRE_FALSE(mayUseQuality({720, 60}, {}));
    REQUIRE(mayUseQuality({1080, 30}, {0, 2}));
    REQUIRE_FALSE(mayUseQuality({1080, 30}, {0, 1}));
    REQUIRE(mayUseQuality({0, 60}, {2, 0}));
    REQUIRE_FALSE(mayUseQuality({0, 60}, {1, 3}));
    REQUIRE_FALSE(mayUseQuality({360, 30}, {2, 3}));
    REQUIRE(usableQuality({1080, 60}, {}) == SendQuality{});
    REQUIRE(atHeight({720, 60}, 480, {}) == SendQuality{480, 30});
    REQUIRE(announcedSize({480, 30}) == FrameSize{640, 480});
    REQUIRE(announcedSize({1440, 60}) == FrameSize{2560, 1440});
    REQUIRE(announcedSize({0, 30}) == FrameSize{});
    REQUIRE(bitrateLimits({720, 30}).max == 3'500'000);
    REQUIRE(bitrateLimits({720, 60}).max == 9'000'000);
    REQUIRE(bitrateLimits({0, 9}).max == 3'500'000);
    REQUIRE(bitrateLimits({0, 10}).max == 9'000'000);
}

TEST_CASE("Viewer-size bitrate scaling preserves upstream floors and picture room", "[video-adaptation]")
{
    constexpr std::int64_t pixels = 1280 * 720;
    const BitrateLimits full{500'000, 600'000, 3'500'000};
    REQUIRE(limitsForViewerSize(full, {720, 30}, pixels, 30, 0) == full);
    REQUIRE(limitsForViewerSize(full, {0, 30}, pixels, 30, pixels / 10) == full);
    REQUIRE(limitsForViewerSize(full, {720, 30}, 900'000, 30, 300'000) == BitrateLimits{166'667, 200'000, 1'166'667});
    REQUIRE(limitsForViewerSize(full, {720, 30}, pixels, 30, pixels / 4) == BitrateLimits{150'000, 150'000, 875'000});
    REQUIRE(limitsForViewerSize(full, {720, 30}, pixels, 30, pixels / 10).max == 552'960);
    REQUIRE(limitsForViewerSize(full, {480, 15}, 640 * 480, 15, 640 * 480 / 100).max == 92'160);
    ViewerSize size;
    REQUIRE_FALSE(size.offer(0)); REQUIRE(size.offer(100'000));
    REQUIRE_FALSE(size.offer(105'000)); REQUIRE(size.offer(105'001));
    REQUIRE(size.offer(0)); REQUIRE(size.pixelCount() == 0);
}

TEST_CASE("Loss controller honors loss intervals, recovery, receiver ceilings, and new limits", "[video-adaptation]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    REQUIRE_FALSE(rate.report(-1, 0)); REQUIRE(rate.bitrate() == 9'000'000);
    REQUIRE_FALSE(rate.report(.10, 0)); REQUIRE(rate.report(.101, 1000)); REQUIRE(rate.bitrate() == 6'300'000);
    REQUIRE_FALSE(rate.report(.5, 2999)); REQUIRE(rate.report(.5, 3000)); REQUIRE(rate.bitrate() == 4'410'000);
    REQUIRE_FALSE(rate.report(.0, 3001)); REQUIRE(rate.report(.0, 13001)); REQUIRE(rate.bitrate() == 5'071'500);
    REQUIRE(rate.setReceiverLimit(2'000'000)); REQUIRE(rate.bitrate() == 2'000'000);
    REQUIRE_FALSE(rate.setReceiverLimit(2'100'000)); REQUIRE(rate.setReceiverLimit(2'300'000)); REQUIRE(rate.bitrate() == 2'300'000);
    REQUIRE(rate.setReceiverLimit(0)); REQUIRE(rate.bitrate() == 5'071'500);
    REQUIRE(rate.setLimits(250'000, 4'500'000)); REQUIRE(rate.bitrate() == 2'535'750);
    rate.restart(500'000, 1'000'000);
    REQUIRE(rate.report(1.0, 0)); REQUIRE(rate.report(1.0, 2000)); REQUIRE(rate.bitrate() == 500'000);
}

TEST_CASE("Camera quality plan uses two-second settling and mute rules", "[video-adaptation]")
{
    REQUIRE(cameraLayerFor(101) == CameraLayer{100, {1280, 720}, 2'500'000});
    REQUIRE(cameraLayerFor(19) == CameraLayer{20, {256, 144}, 150'000});
    REQUIRE(cameraFramerate(20, true) == 12); REQUIRE(cameraFramerate(21, true) == 20);
    CameraSendPlan plan = speakingPlan();
    plan.setWant(50); REQUIRE(plan.advance(1999).size == FrameSize{1280, 720});
    REQUIRE(plan.advance(2000) == CameraEncoding{{416, 234}, 30, 264'062});
    plan.setWant(90); REQUIRE(plan.advance(3999).size == FrameSize{416, 234});
    REQUIRE(plan.advance(4000) == CameraEncoding{{896, 504}, 30, 1'225'000});
    plan.setSelfMuted(true); REQUIRE(plan.advance(4001).framerate == 20);
    plan.setWant(20); REQUIRE(plan.advance(6000).framerate == 12);
    plan.setSharingScreen(true); REQUIRE(plan.advance(6001).framerate == 30);
    plan.setSharingScreen(false); REQUIRE(plan.advance(6002).framerate == 12);
    CameraSendPlan silent; silent.start(1000);
    REQUIRE(silent.nextChangeIn(1000) == std::optional<std::int64_t>(15000));
    REQUIRE(silent.advance(15999).framerate == 30); REQUIRE(silent.advance(16000).framerate == 20);
}

TEST_CASE("Camera bitrate changes settle and retain their maximum share", "[video-adaptation]")
{
    CameraSendPlan plan = speakingPlan();
    plan.reportLoss(.5, 500); REQUIRE(plan.advance(1999).bitrate == 2'500'000);
    REQUIRE(plan.advance(2000).bitrate == 1'750'000);
    plan.setWant(70); REQUIRE(plan.advance(4000) == CameraEncoding{{640, 360}, 30, 437'500});
    plan.reportLoss(.5, 6000); REQUIRE(plan.advance(6000).bitrate == 306'250);
    REQUIRE(plan.nextChangeIn(6000) == std::optional<std::int64_t>{});
}

TEST_CASE("Decode demand halves only persistent backlog and recovers cautiously", "[video-adaptation]")
{
    DecodeDemand demand; std::int64_t now = 0;
    REQUIRE_FALSE(observeFor(demand, now, 2, DecodeDemand::BEHIND_MS));
    REQUIRE(observeFor(demand, now, 1, DecodeDemand::BEHIND_MS)); REQUIRE(demand.pixelShare() == .5);
    REQUIRE_FALSE(observeFor(demand, now, 4, DecodeDemand::BEHIND_MS)); REQUIRE(observeFor(demand, now, 1, DecodeDemand::BEHIND_MS)); REQUIRE(demand.pixelShare() == .25);
    REQUIRE_FALSE(observeFor(demand, now, 60, 0)); REQUIRE(observeFor(demand, now, 1, 0)); REQUIRE(demand.pixelShare() == .5);
    REQUIRE(demand.pixelsToAskFor(3840 * 2160, 1920 * 1080) == (1920 * 1080) / 2);
    DecodeDemand discarded; now = 0; REQUIRE(observeFor(discarded, now, 1, 0, 1)); REQUIRE_FALSE(observeFor(discarded, now, 10, 0, 1));
}

TEST_CASE("Decode demand doubles recovery delay after a failed trial", "[video-adaptation]")
{
    DecodeDemand demand; std::int64_t now = 0;
    observeFor(demand, now, 3, DecodeDemand::BEHIND_MS);
    observeFor(demand, now, 61, 0); REQUIRE(demand.pixelShare() == 1.0);
    observeFor(demand, now, 3, DecodeDemand::BEHIND_MS); REQUIRE(demand.pixelShare() == .5);
    REQUIRE_FALSE(observeFor(demand, now, 120, 0)); REQUIRE(observeFor(demand, now, 1, 0));
}

TEST_CASE("Sink wants preserves the official sender and tile ladders", "[video-adaptation]")
{
    REQUIRE(sinkWantLevel(1) == 100); REQUIRE(sinkWantLevel(3) == 80); REQUIRE(sinkWantLevel(7) == 50); REQUIRE(sinkWantLevel(17) == 20);
    REQUIRE(sinkWantLevel(1, 100'000) == 60); REQUIRE(sinkWantLevel(4, 100'000) == 60);
    SinkWantsInput input{{camera(1, 500), camera(2, 600), camera(3, 700, false)}, 2};
    const auto plan = planSinkWants(input);
    REQUIRE(plan.wants.any == 80); REQUIRE(plan.wants.bySsrc == std::map<std::uint32_t, int>{{500, 0}, {600, 100}, {602, 0}});
    REQUIRE(plan.deliver == std::map<std::uint64_t, std::uint32_t>{{1, 502}, {2, 600}, {3, 700}});
    input.senders[1].wanted = false;
    const auto unwanted = planSinkWants(input); REQUIRE(unwanted.wants.bySsrc.at(600) == 0); REQUIRE(unwanted.wants.bySsrc.at(602) == 0); REQUIRE_FALSE(unwanted.deliver.contains(2));
    SinkWantsInput direct{{camera(1, 500)}, 0, true}; direct.senders[0].tilePixels = 100'000;
    const auto oneToOne = planSinkWants(direct); REQUIRE(oneToOne.wants.bySsrc.at(500) == 0); REQUIRE(oneToOne.wants.bySsrc.at(502) == 60);
}

TEST_CASE("A visible tile is wanted", "[video-adaptation]")
{
    TileVisibility tiles;
    REQUIRE_FALSE(tiles.wanted(1, 0));
    tiles.setVisible(1, true, 0);
    REQUIRE(tiles.wanted(1, 0));
    REQUIRE(tiles.wanted(1, 10 * TileVisibility::HIDDEN_GRACE_MS));
    REQUIRE_FALSE(tiles.hasPendingExpiry(0));
}

TEST_CASE("A hidden tile keeps its video for a while", "[video-adaptation]")
{
    TileVisibility tiles;
    tiles.setVisible(1, true, 0);
    tiles.setVisible(1, false, 1000);
    REQUIRE(tiles.wanted(1, 1000));
    REQUIRE(tiles.wanted(1, 1000 + TileVisibility::HIDDEN_GRACE_MS - 1));
    REQUIRE(tiles.hasPendingExpiry(1000 + TileVisibility::HIDDEN_GRACE_MS - 1));
    REQUIRE_FALSE(tiles.wanted(1, 1000 + TileVisibility::HIDDEN_GRACE_MS));
    REQUIRE_FALSE(tiles.hasPendingExpiry(1000 + TileVisibility::HIDDEN_GRACE_MS));
}

TEST_CASE("Only the most recently hidden tiles are kept", "[video-adaptation]")
{
    TileVisibility tiles;
    for (std::uint64_t user = 1; user <= 5; ++user) {
        tiles.setVisible(user, true, 0);
        tiles.setVisible(user, false, static_cast<std::int64_t>(user) * 100);
    }
    // Hidden in the order 1 to 5: the last three keep their video.
    REQUIRE_FALSE(tiles.wanted(1, 600));
    REQUIRE_FALSE(tiles.wanted(2, 600));
    REQUIRE(tiles.wanted(3, 600));
    REQUIRE(tiles.wanted(4, 600));
    REQUIRE(tiles.wanted(5, 600));
    // One of them coming back makes room for the next most recent.
    tiles.setVisible(5, true, 700);
    REQUIRE(tiles.wanted(2, 700));
    REQUIRE_FALSE(tiles.wanted(1, 700));
}

TEST_CASE("Coming back into view resets the wait", "[video-adaptation]")
{
    TileVisibility tiles;
    tiles.setVisible(1, true, 0);
    tiles.setVisible(1, false, 1000);
    tiles.setVisible(1, true, 20000);
    tiles.setVisible(1, false, 25000);
    REQUIRE(tiles.wanted(1, 1000 + TileVisibility::HIDDEN_GRACE_MS));
    REQUIRE_FALSE(tiles.wanted(1, 25000 + TileVisibility::HIDDEN_GRACE_MS));
    // Being told again that it is hidden does not restart the wait.
    tiles.setVisible(1, false, 40000);
    REQUIRE_FALSE(tiles.wanted(1, 25000 + TileVisibility::HIDDEN_GRACE_MS));
}
