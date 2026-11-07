// Behavioral regression tests ported from ouwou/Acheron's tst_SinkWants.cpp,
// tst_DecodeDemand.cpp, and tst_SendQuality.cpp video-streams tests.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Core/Media/DecodeDemand.hpp"
#include "Core/Video/SendQuality.hpp"
#include "Core/Video/SendRateController.hpp"
#include "Core/Video/TileVisibility.hpp"
#include "Core/Video/VideoTypes.hpp"
#include "Discord/Voice/SinkWants.hpp"

using namespace Styx;
using namespace Styx::Core::Video;
using Styx::Core::Media::DecodeDemand;
using Styx::Core::TileVisibility;
using namespace Styx::Discord::Voice;

// ============================================================================
// Sink Wants & Tile Visibility (ported from Acheron's tests/tst_SinkWants.cpp)
// ============================================================================

namespace {

VideoSender camera(std::uint64_t userId, std::uint32_t highSsrc, bool twoLayers = true)
{
    VideoSender sender;
    sender.userId = userId;
    sender.layers.push_back(VideoLayer{ highSsrc, highSsrc + 1, 100 });
    if (twoLayers)
        sender.layers.push_back(VideoLayer{ highSsrc + 2, highSsrc + 3, 50 });
    return sender;
}

} // namespace

TEST_CASE("Ladder follows the sender count", "[video-adaptation][sink-wants]")
{
    struct Row { int senders; int level; };
    const Row rows[] = {
        { 1, 100 },
        { 2, 90 },
        { 3, 80 },
        { 4, 70 },
        { 5, 60 },
        { 6, 60 },
        { 7, 50 },
        { 9, 50 },
        { 10, 40 },
        { 11, 40 },
        { 12, 30 },
        { 16, 30 },
        { 17, 20 },
        { 25, 20 },
        { 40, 20 },
    };

    for (const auto &row : rows) {
        INFO("senders: " << row.senders);
        REQUIRE(sinkWantLevel(row.senders) == row.level);
    }
}

TEST_CASE("Ladder steps down for a small tile", "[video-adaptation][sink-wants]")
{
    // The first step at least as large as the tile, however few are sending.
    REQUIRE(sinkWantLevel(1, 100'000) == 60);
    REQUIRE(sinkWantLevel(1, 36'863) == 20);
    REQUIRE(sinkWantLevel(1, 921'600) == 100);
    REQUIRE(sinkWantLevel(1, 5'000'000) == 100);
    // Never above what the sender count allows.
    REQUIRE(sinkWantLevel(4, 900'000) == 70);
    REQUIRE(sinkWantLevel(4, 100'000) == 60);
}

TEST_CASE("Nobody sending", "[video-adaptation][sink-wants]")
{
    const SinkWantsPlan plan = planSinkWants({});
    REQUIRE(plan.wants == SinkWantsPayload{ .any = 100, .bySsrc = {} });
    REQUIRE(plan.deliver.empty());
}

TEST_CASE("One sender with one encoding", "[video-adaptation][sink-wants]")
{
    SinkWantsInput input;
    input.senders = { camera(1, 500, false) };

    const SinkWantsPlan plan = planSinkWants(input);
    REQUIRE(plan.wants == SinkWantsPayload{ .any = 100, .bySsrc = {} });
    REQUIRE(plan.deliver.contains(1));
    REQUIRE(plan.deliver.at(1) == 500u);
}

TEST_CASE("One sender with two encodings", "[video-adaptation][sink-wants]")
{
    SinkWantsInput input;
    input.senders = { camera(1, 500) };

    // The large encoding is turned off and the small one falls under `any`.
    const SinkWantsPlan plan = planSinkWants(input);
    REQUIRE(plan.wants == SinkWantsPayload{ .any = 100, .bySsrc = {{500, 0}} });
    REQUIRE(plan.deliver.contains(1));
    REQUIRE(plan.deliver.at(1) == 502u);
}

TEST_CASE("Several senders share the budget", "[video-adaptation][sink-wants]")
{
    SinkWantsInput input;
    input.senders = { camera(1, 500), camera(2, 600), camera(3, 700) };

    const SinkWantsPlan plan = planSinkWants(input);
    REQUIRE(plan.wants == SinkWantsPayload{ .any = 80, .bySsrc = {{500, 0}, {600, 0}, {700, 0}} });
    REQUIRE(plan.deliver.at(1) == 502u);
    REQUIRE(plan.deliver.at(2) == 602u);
    REQUIRE(plan.deliver.at(3) == 702u);
}

TEST_CASE("Focused sender gets full quality", "[video-adaptation][sink-wants]")
{
    SinkWantsInput input;
    input.senders = { camera(1, 500), camera(2, 600), camera(3, 700, false) };
    input.focusedUser = 2;

    SinkWantsPlan plan = planSinkWants(input);
    REQUIRE(plan.wants == SinkWantsPayload{ .any = 80, .bySsrc = {{500, 0}, {600, 100}, {602, 0}} });
    REQUIRE(plan.deliver.at(1) == 502u);
    REQUIRE(plan.deliver.at(2) == 600u);
    REQUIRE(plan.deliver.at(3) == 700u);

    // A sender with a single encoding is asked for it at full quality.
    input.focusedUser = 3;
    plan = planSinkWants(input);
    REQUIRE(plan.wants == SinkWantsPayload{ .any = 80, .bySsrc = {{500, 0}, {600, 0}, {700, 100}} });
    REQUIRE(plan.deliver.at(3) == 700u);
}

TEST_CASE("Focus changes nothing when everyone is at full quality", "[video-adaptation][sink-wants]")
{
    SinkWantsInput input;
    input.senders = { camera(1, 500) };
    input.focusedUser = 1;

    const SinkWantsPlan plan = planSinkWants(input);
    REQUIRE(plan.wants == SinkWantsPayload{ .any = 100, .bySsrc = {{500, 0}} });
    REQUIRE(plan.deliver.at(1) == 502u);
}

TEST_CASE("Unwanted sender gets nothing", "[video-adaptation][sink-wants]")
{
    SinkWantsInput input;
    input.senders = { camera(1, 500), camera(2, 600) };
    input.senders[1].wanted = false;

    // They still count as a sender for everyone else's share.
    const SinkWantsPlan plan = planSinkWants(input);
    REQUIRE(plan.wants == SinkWantsPayload{ .any = 90, .bySsrc = {{500, 0}, {600, 0}, {602, 0}} });
    REQUIRE(plan.deliver.contains(1));
    REQUIRE(!plan.deliver.contains(2));
}

TEST_CASE("Call of two sizes the request to the tile", "[video-adaptation][sink-wants]")
{
    SinkWantsInput input;
    input.senders = { camera(1, 500) };
    input.senders[0].tilePixels = 100'000;
    input.oneToOne = true;

    SinkWantsPlan plan = planSinkWants(input);
    REQUIRE(plan.wants == SinkWantsPayload{ .any = 100, .bySsrc = {{500, 0}, {502, 60}} });

    input.senders = { camera(1, 500, false) };
    input.senders[0].tilePixels = 100'000;
    plan = planSinkWants(input);
    REQUIRE(plan.wants == SinkWantsPayload{ .any = 100, .bySsrc = {{500, 60}} });
}

TEST_CASE("A visible tile is wanted", "[video-adaptation][sink-wants]")
{
    TileVisibility tiles;
    REQUIRE_FALSE(tiles.wanted(1, 0));
    tiles.setVisible(1, true, 0);
    REQUIRE(tiles.wanted(1, 0));
    REQUIRE(tiles.wanted(1, 10 * TileVisibility::HIDDEN_GRACE_MS));
    REQUIRE_FALSE(tiles.hasPendingExpiry(0));
}

TEST_CASE("A hidden tile keeps its video for a while", "[video-adaptation][sink-wants]")
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

TEST_CASE("Only the most recently hidden tiles are kept", "[video-adaptation][sink-wants]")
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

TEST_CASE("Coming back into view resets the wait", "[video-adaptation][sink-wants]")
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

// ============================================================================
// Decode Demand (ported from Acheron's tests/tst_DecodeDemand.cpp)
// ============================================================================

namespace {

constexpr int KEEPING_UP = 0;
constexpr int BEHIND = DecodeDemand::BEHIND_MS;
constexpr int FULL_HD = 1920 * 1080;

// Reports the same decoder state once a second for `seconds`, and says whether the share asked
// for changed at any point.
bool observeFor(DecodeDemand &demand, std::int64_t &nowMs, int seconds, int backlogMs, std::uint64_t discards = 0)
{
    bool changed = false;
    for (int second = 0; second < seconds; ++second) {
        nowMs += 1000;
        changed = demand.observe(backlogMs, discards, nowMs) || changed;
    }
    return changed;
}

} // namespace

TEST_CASE("A short backlog changes nothing", "[video-adaptation][decode-demand]")
{
    DecodeDemand demand;
    std::int64_t now = 0;
    REQUIRE(!observeFor(demand, now, 30, KEEPING_UP));
    REQUIRE(!observeFor(demand, now, 1, BEHIND));
    REQUIRE(!observeFor(demand, now, 30, KEEPING_UP));
    REQUIRE(demand.pixelShare() == 1.0);
}

TEST_CASE("Staying behind halves what is asked for", "[video-adaptation][decode-demand]")
{
    DecodeDemand demand;
    std::int64_t now = 0;
    REQUIRE(!observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000, BEHIND));
    REQUIRE(observeFor(demand, now, 1, BEHIND));
    REQUIRE(demand.pixelShare() == 0.5);
}

TEST_CASE("The sender is given time before the next halving", "[video-adaptation][decode-demand]")
{
    DecodeDemand demand;
    std::int64_t now = 0;
    REQUIRE(observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000 + 1, BEHIND));
    REQUIRE(!observeFor(demand, now, DecodeDemand::SETTLE_MS / 1000 - 1, BEHIND));
    REQUIRE(observeFor(demand, now, 1, BEHIND));
    REQUIRE(demand.pixelShare() == 0.25);

    // And no further than that, however long it goes on.
    REQUIRE(!observeFor(demand, now, 60, BEHIND));
    REQUIRE(demand.pixelShare() == 0.25);
}

TEST_CASE("A discarded queue halves at once", "[video-adaptation][decode-demand]")
{
    DecodeDemand demand;
    std::int64_t now = 0;
    REQUIRE(!observeFor(demand, now, 5, KEEPING_UP));
    REQUIRE(observeFor(demand, now, 1, KEEPING_UP, 1));
    REQUIRE(demand.pixelShare() == 0.5);
    // The same discard is not counted twice.
    REQUIRE(!observeFor(demand, now, 10, KEEPING_UP, 1));
}

TEST_CASE("Keeping up raises it again", "[video-adaptation][decode-demand]")
{
    DecodeDemand demand;
    std::int64_t now = 0;
    observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000 + 1, BEHIND);
    REQUIRE(demand.pixelShare() == 0.5);

    REQUIRE(!observeFor(demand, now, DecodeDemand::FIRST_RAISE_AFTER_MS / 1000, KEEPING_UP));
    REQUIRE(observeFor(demand, now, 1, KEEPING_UP));
    REQUIRE(demand.pixelShare() == 1.0);
}

TEST_CASE("A raise taken back makes the next one wait longer", "[video-adaptation][decode-demand]")
{
    DecodeDemand demand;
    std::int64_t now = 0;
    observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000 + 1, BEHIND);
    observeFor(demand, now, DecodeDemand::FIRST_RAISE_AFTER_MS / 1000 + 1, KEEPING_UP);
    REQUIRE(demand.pixelShare() == 1.0);

    // The full picture is too much for the decoder after all.
    observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000 + 1, BEHIND);
    REQUIRE(demand.pixelShare() == 0.5);

    REQUIRE(!observeFor(demand, now, 2 * DecodeDemand::FIRST_RAISE_AFTER_MS / 1000, KEEPING_UP));
    REQUIRE(observeFor(demand, now, 1, KEEPING_UP));
    REQUIRE(demand.pixelShare() == 1.0);
}

TEST_CASE("Half of what is sent, not of the surface", "[video-adaptation][decode-demand]")
{
    DecodeDemand demand;
    constexpr int SURFACE = 3840 * 2160;
    REQUIRE(demand.pixelsToAskFor(SURFACE, FULL_HD) == SURFACE);

    std::int64_t now = 0;
    observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000 + 1, BEHIND);
    REQUIRE(demand.pixelsToAskFor(SURFACE, FULL_HD) == FULL_HD / 2);
    REQUIRE(demand.pixelsToAskFor(FULL_HD / 4, FULL_HD) == FULL_HD / 8);
    // Before the first frame says how large the picture is.
    REQUIRE(demand.pixelsToAskFor(SURFACE, 0) == SURFACE / 2);
}

// ============================================================================
// Send Quality, Rate Controller, & Camera Plan (ported from Acheron's tests/tst_SendQuality.cpp)
// ============================================================================

namespace {

constexpr int SOURCE = SendQuality::SOURCE_HEIGHT;
constexpr std::int64_t PIXELS_720P = 1280 * 720;

const BitrateLimits FULL_720P{ 500'000, 600'000, 3'500'000 };
const BitrateLimits FULL_1080P{ 500'000, 600'000, 9'000'000 };

// allowed[premium_type][premium_tier]
using TierTable = std::array<std::array<bool, 4>, 4>;

constexpr TierTable EVERYONE{ { { true, true, true, true }, { true, true, true, true }, { true, true, true, true }, { true, true, true, true } } };
constexpr TierTable BOOST_ONE_OR_NITRO{ { { false, true, true, true }, { true, true, true, true }, { true, true, true, true }, { false, true, true, true } } };
constexpr TierTable BOOST_TWO_OR_NITRO{ { { false, false, true, true }, { true, true, true, true }, { true, true, true, true }, { false, false, true, true } } };
constexpr TierTable FULL_NITRO_ONLY{ { { false, false, false, false }, { false, false, false, false }, { true, true, true, true }, { false, false, false, false } } };

const std::array<CameraLayer, 9> CAMERA_LADDER{ {
    { 100, FrameSize{ 1280, 720 }, 2'500'000 },
    { 90, FrameSize{ 896, 504 }, 1'225'000 },
    { 80, FrameSize{ 672, 378 }, 689'062 },
    { 70, FrameSize{ 640, 360 }, 625'000 },
    { 60, FrameSize{ 512, 288 }, 400'000 },
    { 50, FrameSize{ 416, 234 }, 264'062 },
    { 40, FrameSize{ 384, 216 }, 225'000 },
    { 30, FrameSize{ 320, 180 }, 156'250 },
    { 20, FrameSize{ 256, 144 }, 150'000 },
} };

const FrameSize FULL_CAMERA{ 1280, 720 };

// A camera whose owner goes on speaking, so that only what a test does lowers its frame rate.
CameraSendPlan planOfASpeaker()
{
    CameraSendPlan plan;
    plan.start(0);
    plan.setSpeaking(true, 0);
    return plan;
}

} // namespace

TEST_CASE("Quality is gated by Nitro and boosts", "[video-adaptation][send-quality]")
{
    struct QualityRow {
        int height;
        int framerate;
        TierTable allowed;
    };

    std::vector<QualityRow> rows;
    for (const int height : { 480, 720 }) {
        rows.push_back({ height, 15, EVERYONE });
        rows.push_back({ height, 30, EVERYONE });
        rows.push_back({ height, 60, BOOST_ONE_OR_NITRO });
    }
    for (const int height : { 1080, 1440 }) {
        for (const int framerate : { 15, 30, 60 })
            rows.push_back({ height, framerate, BOOST_TWO_OR_NITRO });
    }
    for (const int framerate : { 15, 30, 60 })
        rows.push_back({ SOURCE, framerate, FULL_NITRO_ONLY });

    for (const auto &row : rows) {
        for (int premiumType = 0; premiumType < 4; ++premiumType) {
            for (int boostTier = 0; boostTier < 4; ++boostTier) {
                const bool may = mayUseQuality({ row.height, row.framerate }, { premiumType, boostTier });
                INFO("height: " << row.height << ", framerate: " << row.framerate
                     << ", premiumType: " << premiumType << ", boostTier: " << boostTier);
                REQUIRE(may == row.allowed[premiumType][boostTier]);
            }
        }
    }
}

TEST_CASE("Unlisted heights and framerates are refused", "[video-adaptation][send-quality]")
{
    const SharerTier nitro{ 2, 3 };
    REQUIRE(!mayUseQuality({ 720, 5 }, nitro));
    REQUIRE(!mayUseQuality({ 720, 24 }, nitro));
    REQUIRE(!mayUseQuality({ 360, 30 }, nitro));
    REQUIRE(!mayUseQuality({ 2160, 30 }, nitro));
}

TEST_CASE("A quality the account may not use falls back to the default", "[video-adaptation][send-quality]")
{
    const SendQuality standard;
    REQUIRE(standard.height == 720);
    REQUIRE(standard.framerate == 30);

    REQUIRE(usableQuality({ 1080, 60 }, {}) == standard);
    REQUIRE(usableQuality({ SOURCE, 30 }, { 1, 3 }) == standard);
    REQUIRE(usableQuality({ 1080, 60 }, { 1, 0 }) == SendQuality{ 1080, 60 });
    REQUIRE(usableQuality({ 480, 15 }, {}) == SendQuality{ 480, 15 });
}

TEST_CASE("Moving to another height keeps the framerate where allowed", "[video-adaptation][send-quality]")
{
    const SharerTier boostedTwice{ 0, 2 };
    REQUIRE(atHeight({ 720, 60 }, 1080, boostedTwice) == SendQuality{ 1080, 60 });

    const SharerTier boostedOnce{ 0, 1 };
    REQUIRE(atHeight({ 1080, 15 }, 720, boostedOnce) == SendQuality{ 720, 15 });
    REQUIRE(atHeight({ 720, 60 }, 480, boostedOnce) == SendQuality{ 480, 60 });
    REQUIRE(atHeight({ 720, 60 }, 480, {}) == SendQuality{ 480, 30 });
}

TEST_CASE("Announced size is four by three at 480 and sixteen by nine above", "[video-adaptation][send-quality]")
{
    REQUIRE(announcedSize({ 480, 30 }) == FrameSize{ 640, 480 });
    REQUIRE(announcedSize({ 720, 30 }) == FrameSize{ 1280, 720 });
    REQUIRE(announcedSize({ 1080, 60 }) == FrameSize{ 1920, 1080 });
    REQUIRE(announcedSize({ 1440, 15 }) == FrameSize{ 2560, 1440 });
    REQUIRE(announcedSize({ SOURCE, 30 }) == FrameSize{ 0, 0 });
}

TEST_CASE("Max bitrate follows height and framerate", "[video-adaptation][send-quality]")
{
    struct Row { int height; int framerate; int maxBitrate; };
    const Row rows[] = {
        { 480, 15, 3'500'000 },
        { 720, 30, 3'500'000 },
        { 721, 30, 9'000'000 },
        { 720, 31, 9'000'000 },
        { 720, 60, 9'000'000 },
        { 480, 60, 9'000'000 },
        { 1080, 15, 9'000'000 },
        { 1440, 60, 9'000'000 },
        { SOURCE, 9, 3'500'000 },
        { SOURCE, 10, 9'000'000 },
        { SOURCE, 15, 9'000'000 },
        { SOURCE, 60, 9'000'000 },
    };

    for (const auto &row : rows) {
        INFO("height: " << row.height << ", framerate: " << row.framerate);
        const BitrateLimits limits = bitrateLimits({ row.height, row.framerate });
        REQUIRE(limits.max == row.maxBitrate);
        REQUIRE(limits.min == 500'000);
        REQUIRE(limits.target == 600'000);
    }
}

TEST_CASE("Bitrates are not scaled without a smaller viewer", "[video-adaptation][send-quality]")
{
    const SendQuality quality{ 720, 30 };
    REQUIRE(limitsForViewerSize(FULL_720P, quality, PIXELS_720P, 30, 0) == FULL_720P);
    REQUIRE(limitsForViewerSize(FULL_720P, quality, PIXELS_720P, 30, PIXELS_720P) == FULL_720P);
    REQUIRE(limitsForViewerSize(FULL_720P, quality, PIXELS_720P, 30, PIXELS_720P * 4) == FULL_720P);
    REQUIRE(limitsForViewerSize(FULL_720P, quality, PIXELS_720P, 30, PIXELS_720P - 1) != FULL_720P);
}

TEST_CASE("Source quality is never scaled", "[video-adaptation][send-quality]")
{
    const BitrateLimits full = bitrateLimits({ SOURCE, 30 });
    REQUIRE(limitsForViewerSize(full, { SOURCE, 30 }, 2560 * 1440, 30, 320 * 180) == full);
}

TEST_CASE("Scaled bitrates are rounded up", "[video-adaptation][send-quality]")
{
    // A third of the picture: 166,666.67, 200,000 and 1,166,666.67 before rounding.
    const BitrateLimits scaled = limitsForViewerSize(FULL_720P, { 720, 30 }, 900'000, 30, 300'000);
    REQUIRE(scaled.min == 166'667);
    REQUIRE(scaled.target == 200'000);
    REQUIRE(scaled.max == 1'166'667);
}

TEST_CASE("Scaled minimum and target stop at the floor", "[video-adaptation][send-quality]")
{
    // A quarter of the picture would put the minimum at 125,000 and leaves the target at
    // exactly 150,000.
    const BitrateLimits quarter = limitsForViewerSize(FULL_720P, { 720, 30 }, PIXELS_720P, 30, PIXELS_720P / 4);
    REQUIRE(quarter.min == 150'000);
    REQUIRE(quarter.target == 150'000);

    const BitrateLimits tenth = limitsForViewerSize(FULL_720P, { 720, 30 }, PIXELS_720P, 30, PIXELS_720P / 10);
    REQUIRE(tenth.min == 150'000);
    REQUIRE(tenth.target == 150'000);

    const BitrateLimits half = limitsForViewerSize(FULL_720P, { 720, 30 }, PIXELS_720P, 30, PIXELS_720P / 2);
    REQUIRE(half.min == 250'000);
    REQUIRE(half.target == 300'000);
}

TEST_CASE("Scaled maximum keeps room for the picture", "[video-adaptation][send-quality]")
{
    // 0.02 x 921,600 pixels x 30 frames = 552,960, above a tenth of 3,500,000.
    const BitrateLimits tenth = limitsForViewerSize(FULL_720P, { 720, 30 }, PIXELS_720P, 30, PIXELS_720P / 10);
    REQUIRE(tenth.max == 552'960);

    // And under a half of it.
    const BitrateLimits half = limitsForViewerSize(FULL_720P, { 720, 30 }, PIXELS_720P, 30, PIXELS_720P / 2);
    REQUIRE(half.max == 1'750'000);
}

TEST_CASE("Scaled maximum never passes the full maximum", "[video-adaptation][send-quality]")
{
    // 0.02 x 3,686,400 pixels x 60 frames is 4,423,680, more than this picture may use at all.
    const std::int64_t pixels = 2560 * 1440;
    const BitrateLimits scaled = limitsForViewerSize(FULL_720P, { 1440, 60 }, pixels, 60, pixels / 100);
    REQUIRE(scaled.max == 3'500'000);

    const BitrateLimits roomy = limitsForViewerSize(FULL_1080P, { 1440, 60 }, pixels, 60, pixels / 100);
    REQUIRE(roomy.max == 4'423'680);
}

TEST_CASE("First viewer size is taken up", "[video-adaptation][send-quality]")
{
    ViewerSize size;
    REQUIRE(size.pixelCount() == 0);
    REQUIRE(!size.offer(0));
    REQUIRE(size.offer(1));
    REQUIRE(size.pixelCount() == 1);
}

TEST_CASE("Viewer size within five percent is ignored", "[video-adaptation][send-quality]")
{
    ViewerSize size;
    REQUIRE(size.offer(100'000));

    REQUIRE(!size.offer(100'000));
    REQUIRE(!size.offer(105'000));
    REQUIRE(!size.offer(95'000));
    REQUIRE(size.pixelCount() == 100'000);

    REQUIRE(size.offer(105'001));
    REQUIRE(size.pixelCount() == 105'001);

    ViewerSize shrinking;
    REQUIRE(shrinking.offer(100'000));
    REQUIRE(shrinking.offer(94'999));
    REQUIRE(shrinking.pixelCount() == 94'999);
}

TEST_CASE("Viewer size is compared with the one in use", "[video-adaptation][send-quality]")
{
    // Small steps that are each ignored do not add up to a change that is not.
    ViewerSize size;
    REQUIRE(size.offer(100'000));
    REQUIRE(!size.offer(104'000));
    REQUIRE(!size.offer(105'000));
    REQUIRE(size.offer(108'000));
    REQUIRE(!size.offer(104'000));
}

TEST_CASE("Viewers going away clears the size", "[video-adaptation][send-quality]")
{
    ViewerSize size;
    REQUIRE(size.offer(100'000));
    REQUIRE(size.offer(0));
    REQUIRE(size.pixelCount() == 0);
}

TEST_CASE("Controller ignores reports before the server has said", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    for (std::int64_t now = 0; now <= 30'000; now += 1000)
        REQUIRE(!rate.report(-1, now));
    REQUIRE(rate.bitrate() == 9'000'000);
}

TEST_CASE("Heavy loss lowers the bitrate", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);

    REQUIRE(!rate.report(0.10, 0));
    REQUIRE(rate.bitrate() == 9'000'000);

    REQUIRE(rate.report(0.101, 1000));
    REQUIRE(rate.bitrate() == 6'300'000);
}

TEST_CASE("Heavy loss lowers no more often than every two seconds", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);

    REQUIRE(rate.report(0.5, 5000));
    REQUIRE(!rate.report(0.5, 6000));
    REQUIRE(!rate.report(0.5, 6999));
    REQUIRE(rate.bitrate() == 6'300'000);

    REQUIRE(rate.report(0.5, 7000));
    REQUIRE(rate.bitrate() == 4'410'000);
    REQUIRE(!rate.report(0.5, 8000));
    REQUIRE(rate.report(0.5, 9000));
    REQUIRE(rate.bitrate() == 3'087'000);
}

TEST_CASE("Bitrate never goes under the minimum", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 1'000'000);

    REQUIRE(rate.report(1.0, 0));
    REQUIRE(rate.bitrate() == 700'000);
    REQUIRE(rate.report(1.0, 2000));
    REQUIRE(rate.bitrate() == 500'000);
    REQUIRE(!rate.report(1.0, 4000));
    REQUIRE(!rate.report(1.0, 6000));
    REQUIRE(rate.bitrate() == 500'000);
}

TEST_CASE("Moderate loss holds", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    REQUIRE(rate.report(0.5, 0));

    for (std::int64_t now = 1000; now <= 60'000; now += 1000) {
        const double loss = now % 2000 == 0 ? 0.02 : 0.10;
        REQUIRE(!rate.report(loss, now));
    }
    REQUIRE(rate.bitrate() == 6'300'000);
}

TEST_CASE("Ten seconds without loss raises the bitrate", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    REQUIRE(rate.report(0.5, 0));
    REQUIRE(rate.report(0.5, 2000));
    REQUIRE(rate.bitrate() == 4'410'000);

    for (std::int64_t now = 3000; now < 13'000; now += 1000)
        REQUIRE(!rate.report(0.019, now));
    REQUIRE(rate.report(0.0, 13'000));
    REQUIRE(rate.bitrate() == 5'071'500);
}

TEST_CASE("Each raise needs another ten clean seconds", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    REQUIRE(rate.report(0.5, 0));
    REQUIRE(rate.report(0.5, 2000));

    REQUIRE(!rate.report(0, 3000));
    REQUIRE(rate.report(0, 13'000));
    REQUIRE(!rate.report(0, 14'000));
    REQUIRE(!rate.report(0, 22'999));
    REQUIRE(rate.bitrate() == 5'071'500);

    REQUIRE(rate.report(0, 23'000));
    REQUIRE(rate.bitrate() == 5'832'225);
}

TEST_CASE("Raises stop at the maximum", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    REQUIRE(rate.report(0.5, 0));
    REQUIRE(rate.bitrate() == 6'300'000);

    REQUIRE(!rate.report(0, 1000));
    REQUIRE(rate.report(0, 11'000));
    REQUIRE(rate.bitrate() == 7'245'000);
    REQUIRE(rate.report(0, 21'000));
    REQUIRE(rate.bitrate() == 8'331'750);
    REQUIRE(rate.report(0, 31'000));
    REQUIRE(rate.bitrate() == 9'000'000);

    REQUIRE(!rate.report(0, 41'000));
    REQUIRE(!rate.report(0, 51'000));
    REQUIRE(rate.bitrate() == 9'000'000);
}

TEST_CASE("Loss interrupts the clean stretch", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    REQUIRE(rate.report(0.5, 0));

    REQUIRE(!rate.report(0, 1000));
    REQUIRE(!rate.report(0.05, 9000));
    REQUIRE(!rate.report(0, 10'000));
    REQUIRE(!rate.report(0, 19'999));
    REQUIRE(rate.bitrate() == 6'300'000);

    REQUIRE(rate.report(0, 20'000));
    REQUIRE(rate.bitrate() == 7'245'000);
}

TEST_CASE("The receiver's estimate is a ceiling", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);

    // One above what is in use changes nothing.
    REQUIRE(!rate.setReceiverLimit(20'000'000));
    REQUIRE(rate.bitrate() == 9'000'000);

    REQUIRE(rate.setReceiverLimit(3'000'000));
    REQUIRE(rate.bitrate() == 3'000'000);

    // Never under the minimum, whatever it says.
    REQUIRE(rate.setReceiverLimit(100'000));
    REQUIRE(rate.bitrate() == 500'000);

    // New limits for the video are bounded by it as well.
    rate.setReceiverLimit(0);
    REQUIRE(rate.setReceiverLimit(3'000'000));
    REQUIRE(!rate.setLimits(500'000, 6'000'000));
    REQUIRE(rate.bitrate() == 3'000'000);
    REQUIRE(rate.setLimits(500'000, 2'000'000));
    REQUIRE(rate.bitrate() == 2'000'000);

    // Starting over forgets it.
    rate.restart(500'000, 9'000'000);
    REQUIRE(rate.bitrate() == 9'000'000);
}

TEST_CASE("The receiver's estimate is followed down at once and up by whole steps", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    REQUIRE(rate.setReceiverLimit(4'000'000));

    REQUIRE(rate.setReceiverLimit(3'990'000));
    REQUIRE(rate.bitrate() == 3'990'000);

    // An estimate that wanders about the same place does not have the encoder restarted for it.
    for (int estimate : { 4'000'000, 4'300'000, 3'995'000, 4'580'000 })
        REQUIRE(!rate.setReceiverLimit(estimate));
    REQUIRE(rate.bitrate() == 3'990'000);

    REQUIRE(rate.setReceiverLimit(4'600'000));
    REQUIRE(rate.bitrate() == 4'600'000);
}

TEST_CASE("Lifting the receiver's estimate leaves no mark", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    REQUIRE(rate.setReceiverLimit(1'000'000));

    REQUIRE(rate.setReceiverLimit(0));
    REQUIRE(rate.bitrate() == 9'000'000);
    REQUIRE(!rate.setReceiverLimit(0));
}

TEST_CASE("Loss lowers from what the receiver's estimate allows", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    REQUIRE(rate.setReceiverLimit(2'000'000));

    // Not from the nine megabits that losses alone had allowed, which would change nothing.
    REQUIRE(rate.report(0.5, 0));
    REQUIRE(rate.bitrate() == 1'400'000);

    // A clean stretch then raises it as ever, and the estimate still bounds that.
    for (std::int64_t now = 1000; now < 11'000; now += 1000)
        REQUIRE(!rate.report(0.0, now));
    REQUIRE(rate.report(0.0, 11'000));
    REQUIRE(rate.bitrate() == 1'610'000);
    REQUIRE(rate.report(0.0, 21'000));
    REQUIRE(rate.report(0.0, 31'000));
    REQUIRE(rate.bitrate() == 2'000'000);
    REQUIRE(!rate.report(0.0, 41'000));
}

TEST_CASE("New limits keep the share of the maximum", "[video-adaptation][send-quality]")
{
    SendRateController untouched;
    untouched.restart(500'000, 9'000'000);
    REQUIRE(untouched.setLimits(250'000, 4'500'000));
    REQUIRE(untouched.bitrate() == 4'500'000);
    REQUIRE(untouched.setLimits(500'000, 9'000'000));
    REQUIRE(untouched.bitrate() == 9'000'000);
    REQUIRE(!untouched.setLimits(500'000, 9'000'000));

    SendRateController lowered;
    lowered.restart(500'000, 9'000'000);
    REQUIRE(lowered.report(0.5, 0));
    REQUIRE(lowered.setLimits(250'000, 4'500'000));
    REQUIRE(lowered.bitrate() == 3'150'000);

    // The new minimum still holds.
    REQUIRE(lowered.setLimits(400'000, 500'000));
    REQUIRE(lowered.bitrate() == 400'000);
}

TEST_CASE("A maximum under the minimum is still a maximum", "[video-adaptation][send-quality]")
{
    // What the viewer-size rule gives a 480p15 picture shown very small.
    const BitrateLimits scaled = limitsForViewerSize(FULL_720P, { 480, 15 }, 640 * 480, 15, 640 * 480 / 100);
    REQUIRE(scaled.min == 150'000);
    REQUIRE(scaled.max == 92'160);

    SendRateController rate;
    rate.restart(scaled.min, scaled.max);
    REQUIRE(rate.bitrate() == 92'160);
    REQUIRE(!rate.report(0.5, 0));
    REQUIRE(rate.bitrate() == 92'160);
}

TEST_CASE("Restart goes back to the maximum", "[video-adaptation][send-quality]")
{
    SendRateController rate;
    rate.restart(500'000, 9'000'000);
    REQUIRE(rate.report(0.5, 0));

    rate.restart(500'000, 3'500'000);
    REQUIRE(rate.bitrate() == 3'500'000);
    // The two seconds between decreases start over as well.
    REQUIRE(rate.report(0.5, 100));
    REQUIRE(rate.bitrate() == 2'450'000);
}

TEST_CASE("The lower estimate of the way is its ceiling", "[video-adaptation][send-quality]")
{
    REQUIRE(estimatedCeiling(0, 0) == 0);
    REQUIRE(estimatedCeiling(2'000'000, 0) == 2'000'000);
    REQUIRE(estimatedCeiling(0, 900'000) == 900'000);
    REQUIRE(estimatedCeiling(2'000'000, 900'000) == 900'000);
    REQUIRE(estimatedCeiling(700'000, 900'000) == 700'000);
}

TEST_CASE("Camera ladder has a row for each want", "[video-adaptation][send-quality]")
{
    for (const CameraLayer &row : CAMERA_LADDER) {
        INFO("want: " << row.want);
        const CameraLayer chosen = cameraLayerFor(row.want);
        REQUIRE(chosen.want == row.want);
        REQUIRE(chosen.size == row.size);
        REQUIRE(chosen.maxBitrate == row.maxBitrate);
        REQUIRE(chosen.size.width * 9 == chosen.size.height * 16);
    }
}

TEST_CASE("A want between rows takes the row below", "[video-adaptation][send-quality]")
{
    for (size_t i = 0; i + 1 < CAMERA_LADDER.size(); ++i) {
        const CameraLayer &row = CAMERA_LADDER[i];
        const CameraLayer &below = CAMERA_LADDER[i + 1];
        REQUIRE(cameraLayerFor(row.want - 1) == below);
        REQUIRE(cameraLayerFor(below.want + 1) == below);
    }

    // More than everything is still everything.
    REQUIRE(cameraLayerFor(101) == CAMERA_LADDER.front());
}

TEST_CASE("A want under twenty takes the lowest row", "[video-adaptation][send-quality]")
{
    for (const int want : { 19, 10, 1, 0, -1 })
        REQUIRE(cameraLayerFor(want) == CAMERA_LADDER.back());
}

TEST_CASE("Camera framerate is lowered for a muted owner", "[video-adaptation][send-quality]")
{
    struct Row { int want; bool muted; int framerate; };
    const Row rows[] = {
        { 100, false, 30 },
        { 20, false, 30 },
        { 0, false, 30 },
        { 100, true, 20 },
        { 30, true, 20 },
        { 21, true, 20 },
        { 20, true, 12 },
        { 19, true, 12 },
        { 0, true, 12 },
    };

    for (const auto &row : rows) {
        INFO("want: " << row.want << ", muted: " << row.muted);
        REQUIRE(cameraFramerate(row.want, row.muted) == row.framerate);
    }
}

TEST_CASE("The camera starts with the whole picture at its maximum", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan;
    plan.start(1000);

    const CameraEncoding first = plan.advance(1000);
    REQUIRE(first.size == FULL_CAMERA);
    REQUIRE(first.framerate == 30);
    REQUIRE(first.bitrate == 2'500'000);
    REQUIRE(plan.servedWant() == 100);
    REQUIRE(plan.maxBitrate() == 2'500'000);

    // A camera turned on again starts over, whatever the last one was down to.
    plan.setWant(20);
    plan.reportLoss(0.5, 4000);
    REQUIRE(plan.advance(4000).size == FrameSize{ 256, 144 });
    plan.start(9000);
    REQUIRE(plan.advance(9000) == first);
}

TEST_CASE("Self-mute lowers the framerate at once", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan = planOfASpeaker();
    REQUIRE(plan.advance(100).framerate == 30);

    plan.setSelfMuted(true);
    REQUIRE(plan.countsAsMuted(100));
    const CameraEncoding muted = plan.advance(100);
    REQUIRE(muted.framerate == 20);
    // Nothing else of the picture changes with it.
    REQUIRE(muted.size == FULL_CAMERA);
    REQUIRE(muted.bitrate == 2'500'000);

    plan.setSelfMuted(false);
    REQUIRE(!plan.countsAsMuted(101));
    REQUIRE(plan.advance(101).framerate == 30);
}

TEST_CASE("Fifteen seconds of silence lower the framerate", "[video-adaptation][send-quality]")
{
    // Nothing has been said since the camera was turned on.
    CameraSendPlan plan;
    plan.start(1000);

    REQUIRE(plan.advance(15'999).framerate == 30);
    REQUIRE(!plan.countsAsMuted(15'999));
    REQUIRE(plan.advance(16'000).framerate == 20);
    REQUIRE(plan.countsAsMuted(16'000));
    REQUIRE(plan.advance(600'000).framerate == 20);
}

TEST_CASE("Silence is counted from the last word", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan;
    plan.start(0);
    plan.setSpeaking(true, 5000);
    // Speaking, for however long, keeps the full rate.
    REQUIRE(plan.advance(60'000).framerate == 30);

    plan.setSpeaking(false, 60'000);
    REQUIRE(plan.advance(74'999).framerate == 30);
    REQUIRE(plan.advance(75'000).framerate == 20);

    // Being told again that nothing is said does not start the count over.
    plan.setSpeaking(true, 80'000);
    plan.setSpeaking(false, 81'000);
    plan.setSpeaking(false, 90'000);
    REQUIRE(plan.advance(95'999).framerate == 30);
    REQUIRE(plan.advance(96'000).framerate == 20);
}

TEST_CASE("Speaking restores the framerate at once", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan;
    plan.start(0);
    REQUIRE(plan.advance(20'000).framerate == 20);

    plan.setSpeaking(true, 20'001);
    REQUIRE(plan.advance(20'001).framerate == 30);

    // Not while self-muted, though: what is said then is not heard.
    plan.setSelfMuted(true);
    REQUIRE(plan.advance(20'002).framerate == 20);
}

TEST_CASE("A muted camera wanted at twenty or less is sent at twelve", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan = planOfASpeaker();
    plan.setSelfMuted(true);

    plan.setWant(21);
    const CameraEncoding lowestRow = plan.advance(2000);
    REQUIRE(lowestRow.size == FrameSize{ 256, 144 });
    REQUIRE(lowestRow.framerate == 20);

    // The same row, and still a change for the encoder.
    plan.setWant(20);
    REQUIRE(plan.advance(3999).framerate == 20);
    const CameraEncoding slowest = plan.advance(4000);
    REQUIRE(slowest.size == FrameSize{ 256, 144 });
    REQUIRE(slowest.framerate == 12);

    plan.setSelfMuted(false);
    REQUIRE(plan.advance(4001).framerate == 30);
}

TEST_CASE("The framerate is not lowered while a screen is shared", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan;
    plan.start(0);
    plan.setSelfMuted(true);
    plan.setSharingScreen(true);

    REQUIRE(!plan.countsAsMuted(60'000));
    REQUIRE(plan.advance(60'000).framerate == 30);

    plan.setSharingScreen(false);
    REQUIRE(plan.advance(60'000).framerate == 20);

    // Silence does not lower it either.
    plan.setSelfMuted(false);
    REQUIRE(plan.advance(60'001).framerate == 20);
    plan.setSharingScreen(true);
    REQUIRE(plan.advance(60'002).framerate == 30);
}

TEST_CASE("A new want is taken up two seconds after the last change", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan = planOfASpeaker();

    plan.setWant(50);
    REQUIRE(plan.advance(1999).size == FULL_CAMERA);
    REQUIRE(plan.servedWant() == 100);

    const CameraEncoding lowered = plan.advance(2000);
    REQUIRE(lowered.size == FrameSize{ 416, 234 });
    REQUIRE(lowered.framerate == 30);
    REQUIRE(lowered.bitrate == 264'062);
    REQUIRE(plan.servedWant() == 50);
    REQUIRE(plan.maxBitrate() == 264'062);

    // The two seconds start again with every change that is made.
    plan.setWant(100);
    REQUIRE(plan.advance(3999) == lowered);
    const CameraEncoding raised = plan.advance(4000);
    REQUIRE(raised.size == FULL_CAMERA);
    REQUIRE(raised.bitrate == 2'500'000);
}

TEST_CASE("Wants in quick succession are gathered into one", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan = planOfASpeaker();

    // Long after the last change, the first of them is taken up at once.
    plan.setWant(50);
    REQUIRE(plan.advance(5000).size == FrameSize{ 416, 234 });

    plan.setWant(30);
    REQUIRE(plan.advance(5100).size == FrameSize{ 416, 234 });
    plan.setWant(70);
    REQUIRE(plan.advance(5500).size == FrameSize{ 416, 234 });
    plan.setWant(90);
    REQUIRE(plan.advance(6999).size == FrameSize{ 416, 234 });

    const CameraEncoding settled = plan.advance(7000);
    REQUIRE(settled.size == FrameSize{ 896, 504 });
    REQUIRE(settled.bitrate == 1'225'000);

    // A want that comes back to the one in use leaves nothing to do.
    plan.setWant(40);
    plan.setWant(90);
    REQUIRE(!plan.nextChangeIn(7100).has_value());
    REQUIRE(plan.advance(20'000) == settled);
}

TEST_CASE("A want within the row in use changes nothing", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan = planOfASpeaker();
    plan.setWant(90);
    const CameraEncoding atNinety = plan.advance(2000);

    plan.setWant(95);
    REQUIRE(plan.servedWant() == 95);
    REQUIRE(!plan.nextChangeIn(2100).has_value());
    REQUIRE(plan.advance(2100) == atNinety);

    // And does not put off a change that does matter to the encoder.
    plan.setWant(80);
    REQUIRE(plan.nextChangeIn(2100) == std::optional<std::int64_t>(1900));
    REQUIRE(plan.advance(4000).size == FrameSize{ 672, 378 });
}

TEST_CASE("Loss moves the camera bitrate down to its minimum", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan = planOfASpeaker();

    plan.reportLoss(0.5, 3000);
    REQUIRE(plan.advance(3000).bitrate == 1'750'000);
    plan.reportLoss(0.5, 5000);
    const CameraEncoding lowered = plan.advance(5000);
    REQUIRE(lowered.bitrate == 1'225'000);
    // The picture keeps its size: only the want changes that.
    REQUIRE(lowered.size == FULL_CAMERA);

    // Before the server has said anything of loss there is nothing to act on.
    plan.reportLoss(-1, 7000);
    REQUIRE(plan.advance(7000).bitrate == 1'225'000);

    std::int64_t now = 7000;
    for (int i = 0; i < 12; ++i) {
        now += 2000;
        plan.reportLoss(0.5, now);
        plan.advance(now);
    }
    REQUIRE(plan.advance(now).bitrate == 150'000);
}

TEST_CASE("A bitrate change waits out the two seconds too", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan = planOfASpeaker();

    plan.reportLoss(0.5, 500);
    REQUIRE(plan.advance(500).bitrate == 2'500'000);
    REQUIRE(plan.advance(1999).bitrate == 2'500'000);
    REQUIRE(plan.advance(2000).bitrate == 1'750'000);
}

TEST_CASE("A new row keeps the bitrate's share of its maximum", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan = planOfASpeaker();
    plan.reportLoss(0.5, 3000);
    REQUIRE(plan.advance(3000).bitrate == 1'750'000);

    plan.setWant(70);
    const CameraEncoding smaller = plan.advance(5000);
    REQUIRE(smaller.size == FrameSize{ 640, 360 });
    REQUIRE(smaller.bitrate == 437'500);

    // On the lowest row the minimum and the maximum are one and the same.
    plan.setWant(20);
    REQUIRE(plan.advance(7000).bitrate == 150'000);
    plan.reportLoss(0.5, 9000);
    REQUIRE(plan.advance(9000).bitrate == 150'000);
}

TEST_CASE("The plan says when it next changes by itself", "[video-adaptation][send-quality]")
{
    CameraSendPlan plan;
    plan.start(1000);

    // Silent since the camera came on: the frame rate drops fifteen seconds after that.
    REQUIRE(plan.nextChangeIn(1000) == std::optional<std::int64_t>(15'000));
    REQUIRE(plan.nextChangeIn(10'000) == std::optional<std::int64_t>(6000));

    // A want that waits for the two seconds comes first.
    plan.setWant(50);
    REQUIRE(plan.nextChangeIn(1500) == std::optional<std::int64_t>(1500));
    plan.advance(3000);
    REQUIRE(plan.nextChangeIn(3000) == std::optional<std::int64_t>(13'000));

    // Once the rate has dropped, or while it cannot, nothing is due.
    plan.advance(16'000);
    REQUIRE(!plan.nextChangeIn(16'000).has_value());
    plan.setSpeaking(true, 17'000);
    REQUIRE(!plan.nextChangeIn(17'000).has_value());
    plan.setSpeaking(false, 18'000);
    REQUIRE(plan.nextChangeIn(18'000) == std::optional<std::int64_t>(15'000));
    plan.setSelfMuted(true);
    REQUIRE(!plan.nextChangeIn(18'000).has_value());
    plan.setSelfMuted(false);
    plan.setSharingScreen(true);
    REQUIRE(!plan.nextChangeIn(18'000).has_value());

    // A change that is overdue is due now.
    plan.setWant(100);
    REQUIRE(plan.nextChangeIn(30'000) == std::optional<std::int64_t>(0));
}
