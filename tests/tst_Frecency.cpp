#include <catch2/catch_test_macros.hpp>

#include "Core/Emoji/FrecencyTracker.hpp"

#include <vector>

using namespace Styx::Core;

static constexpr uint64_t DAY = 86400000ULL;
static constexpr uint64_t NOW = 1700000000000ULL;

static FrecencyTracker::Entry makeEntry(uint32_t totalUses, const QList<uint64_t> &recentUses)
{
    FrecencyTracker::Entry entry;
    entry.totalUses = totalUses;
    entry.recentUses = recentUses;
    return entry;
}

TEST_CASE("Frecency: Weight buckets", "[frecency]")
{
    // Boundaries of <=3 / <=15 / <=30 / <=45 / <=80 / else.
    REQUIRE(FrecencyTracker::defaultComputeWeight(0) == 100);
    REQUIRE(FrecencyTracker::defaultComputeWeight(3) == 100);
    REQUIRE(FrecencyTracker::defaultComputeWeight(4) == 70);
    REQUIRE(FrecencyTracker::defaultComputeWeight(15) == 70);
    REQUIRE(FrecencyTracker::defaultComputeWeight(16) == 50);
    REQUIRE(FrecencyTracker::defaultComputeWeight(30) == 50);
    REQUIRE(FrecencyTracker::defaultComputeWeight(31) == 30);
    REQUIRE(FrecencyTracker::defaultComputeWeight(45) == 30);
    REQUIRE(FrecencyTracker::defaultComputeWeight(46) == 10);
    REQUIRE(FrecencyTracker::defaultComputeWeight(80) == 10);
    REQUIRE(FrecencyTracker::defaultComputeWeight(81) == 1);
    REQUIRE(FrecencyTracker::defaultComputeWeight(10000) == 1);

    // A timestamp in the future gives a negative age, which lands in the freshest bucket just as
    // moment's truncating diff does.
    REQUIRE(FrecencyTracker::defaultComputeWeight(-1) == 100);
}

TEST_CASE("Frecency: Age in days", "[frecency]")
{
    REQUIRE(FrecencyTracker::ageInDays(NOW, NOW) == 0);
    REQUIRE(FrecencyTracker::ageInDays(NOW, NOW - DAY) == 1);
    REQUIRE(FrecencyTracker::ageInDays(NOW, NOW - (DAY - 1)) == 0);
    REQUIRE(FrecencyTracker::ageInDays(NOW, NOW - (3 * DAY + 1)) == 3);
    REQUIRE(FrecencyTracker::ageInDays(NOW, NOW - (4 * DAY - 1)) == 3);
    // Truncation toward zero, not floor.
    REQUIRE(FrecencyTracker::ageInDays(NOW, NOW + DAY) == -1);
    REQUIRE(FrecencyTracker::ageInDays(NOW, NOW + DAY / 2) == 0);
}

TEST_CASE("Frecency: Track", "[frecency]")
{
    FrecencyTracker tracker(FrecencyTracker::chatEmojiOptions());
    REQUIRE(!tracker.isDirty());

    tracker.track("thumbsup", NOW - 5 * DAY);
    REQUIRE(tracker.isDirty());
    REQUIRE(tracker.usageHistory().size() == 1);

    const FrecencyTracker::Entry &fresh = tracker.usageHistory().value("thumbsup");
    REQUIRE(fresh.totalUses == 1u);
    REQUIRE(fresh.recentUses == QList<uint64_t>({ NOW - 5 * DAY }));
    REQUIRE(fresh.frecency == -1);
    REQUIRE(fresh.score == 0.0);

    // An existing entry accumulates uses; an explicit timestamp older than what is already there
    // is sorted back into place.
    tracker.track("thumbsup", NOW - 9 * DAY, 3);
    const FrecencyTracker::Entry &merged = tracker.usageHistory().value("thumbsup");
    REQUIRE(merged.totalUses == 4u);
    REQUIRE(merged.recentUses == QList<uint64_t>({ NOW - 9 * DAY, NOW - 5 * DAY }));
    REQUIRE(merged.frecency == -1);

    // Only the newest maxSamples (10) timestamps are kept; the oldest are shifted off the front.
    FrecencyTracker capped;
    for (int i = 0; i < 12; ++i)
        capped.track("eyes", NOW - static_cast<uint64_t>(12 - i) * DAY);

    const FrecencyTracker::Entry &shifted = capped.usageHistory().value("eyes");
    REQUIRE(shifted.totalUses == 12u);
    REQUIRE(shifted.recentUses.size() == 10);
    REQUIRE(shifted.recentUses.first() == NOW - 10 * DAY);
    REQUIRE(shifted.recentUses.last() == NOW - DAY);
}

TEST_CASE("Frecency: Compute", "[frecency]")
{
    FrecencyTracker tracker(FrecencyTracker::chatEmojiOptions());

    QHash<QString, FrecencyTracker::Entry> history;
    history.insert("thumbsup", makeEntry(5, { NOW - 40 * DAY, NOW - 10 * DAY, NOW - DAY }));
    tracker.overwriteHistory(history);

    tracker.compute(NOW);
    REQUIRE(!tracker.isDirty());

    // weight(40) + weight(10) + weight(1) = 30 + 70 + 100
    REQUIRE(tracker.score("thumbsup").value() == 200.0);
    // ceil(5 * (200 / 3)) = ceil(333.33) = 334
    REQUIRE(tracker.frecency("thumbsup").value() == 334);

    REQUIRE(!tracker.score("nope").has_value());
    REQUIRE(!tracker.frecency("nope").has_value());
}

TEST_CASE("Frecency: Old entry survives", "[frecency]")
{
    // Age alone never drops an entry: past 80 days the weight bottoms out at 1, which is still a
    // positive score.
    FrecencyTracker tracker;

    QHash<QString, FrecencyTracker::Entry> history;
    history.insert("ancient", makeEntry(1, { NOW - 400 * DAY }));
    tracker.overwriteHistory(history);
    tracker.compute(NOW);

    REQUIRE(tracker.usageHistory().size() == 1);
    REQUIRE(tracker.score("ancient").value() == 1.0);
    REQUIRE(tracker.frecency("ancient").value() == 1);
}

TEST_CASE("Frecency: Empty recent uses dropped", "[frecency]")
{
    // Score 0 is only reachable with no samples at all, and that entry is deleted.
    FrecencyTracker tracker;

    QHash<QString, FrecencyTracker::Entry> history;
    history.insert("empty", makeEntry(9, {}));
    history.insert("used", makeEntry(1, { NOW - DAY }));
    tracker.overwriteHistory(history);
    tracker.compute(NOW);

    REQUIRE(tracker.usageHistory().size() == 1);
    REQUIRE(!tracker.score("empty").has_value());
    REQUIRE(tracker.frequently(NOW) == QStringList({ "used" }));
}

TEST_CASE("Frecency: Frequently ordering and cap", "[frecency]")
{
    FrecencyTracker tracker(FrecencyTracker::chatEmojiOptions());

    // 50 entries, all used once a day ago (score 100), so frecency == totalUses * 100 and the
    // ordering is exactly the reverse of the index.
    QHash<QString, FrecencyTracker::Entry> history;
    for (int i = 0; i < 50; ++i) {
        history.insert(QStringLiteral("k%1").arg(i, 2, 10, QLatin1Char('0')),
                       makeEntry(static_cast<uint32_t>(i + 1), { NOW - DAY }));
    }
    tracker.overwriteHistory(history);

    REQUIRE(tracker.isDirty());
    const QStringList frequently = tracker.frequently(NOW);
    REQUIRE(!tracker.isDirty());

    REQUIRE(frequently.size() == 42);
    REQUIRE(frequently.first() == QStringLiteral("k49"));
    REQUIRE(frequently.at(1) == QStringLiteral("k48"));
    REQUIRE(frequently.last() == QStringLiteral("k08"));
    REQUIRE(tracker.frecency("k49").value() == 5000);

    // Entries below the cut are still tracked, just not "frequently".
    REQUIRE(tracker.usageHistory().size() == 50);
    REQUIRE(tracker.frecency("k00").value() == 100);
}

TEST_CASE("Frecency: Compute skips clean entries", "[frecency]")
{
    // frecency != -1 means "already computed"; a later compute() must leave it alone, exactly like
    // the client (only track/overwriteHistory invalidate).
    FrecencyTracker tracker;

    QHash<QString, FrecencyTracker::Entry> history;
    history.insert("thumbsup", makeEntry(2, { NOW - DAY }));
    tracker.overwriteHistory(history);
    tracker.compute(NOW);
    REQUIRE(tracker.frecency("thumbsup").value() == 200);

    tracker.compute(NOW + 300 * DAY);
    REQUIRE(tracker.score("thumbsup").value() == 100.0);
    REQUIRE(tracker.frecency("thumbsup").value() == 200);

    // Tracking dirties the entry, so the stale age is now folded in.
    tracker.track("thumbsup", NOW + 300 * DAY);
    tracker.compute(NOW + 300 * DAY);
    // weight(301) + weight(0) = 1 + 100
    REQUIRE(tracker.score("thumbsup").value() == 101.0);
    // ceil(3 * (101 / 2)) = 152
    REQUIRE(tracker.frecency("thumbsup").value() == 152);
}

TEST_CASE("Frecency: Reaction formula", "[frecency]")
{
    FrecencyTracker tracker(FrecencyTracker::reactionEmojiOptions());

    QHash<QString, FrecencyTracker::Entry> history;
    history.insert("hot", makeEntry(3, { NOW - 40 * DAY, NOW - 10 * DAY, NOW - DAY }));
    history.insert("stale", makeEntry(7, { NOW - 100 * DAY }));
    tracker.overwriteHistory(history);
    tracker.compute(NOW);

    // maxTotalUse = 7 (from "stale"), scores are 200 and 1.
    REQUIRE(tracker.score("hot").value() == 200.0);
    REQUIRE(tracker.score("stale").value() == 1.0);
    // trunc(1000 * ((3/7)*0.2 + (200/1000)*0.8)) = trunc(245.71)
    REQUIRE(tracker.frecency("hot").value() == 245);
    // trunc(1000 * ((7/7)*0.2 + (1/1000)*0.8)) = trunc(200.8)
    REQUIRE(tracker.frecency("stale").value() == 200);
    REQUIRE(tracker.frequently(NOW) == QStringList({ "hot", "stale" }));
}

TEST_CASE("Frecency: Cap for persistence", "[frecency]")
{
    QHash<QString, FrecencyTracker::Entry> history;
    FrecencyTracker::Entry newest = makeEntry(1, { NOW - 50 * DAY, NOW - DAY });
    newest.score = 12.4;
    FrecencyTracker::Entry middle = makeEntry(2, { NOW - 5 * DAY });
    middle.score = 7.6;
    FrecencyTracker::Entry oldest = makeEntry(9, { NOW - 90 * DAY });
    oldest.score = 1.0;
    history.insert("newest", newest);
    history.insert("middle", middle);
    history.insert("oldest", oldest);

    const auto capped = FrecencyTracker::capForPersistence(history, 2);
    REQUIRE(capped.size() == 2);
    REQUIRE(capped.contains("newest"));
    REQUIRE(capped.contains("middle"));
    REQUIRE(!capped.contains("oldest"));

    REQUIRE(capped.value("newest").score == 12.0);
    REQUIRE(capped.value("middle").score == 8.0);
    REQUIRE(capped.value("newest").totalUses == 1u);

    // Under the limit nothing is dropped.
    REQUIRE(FrecencyTracker::capForPersistence(history, 100).size() == 3);
}

TEST_CASE("Frecency: Overwrite history", "[frecency]")
{
    FrecencyTracker tracker;

    QHash<QString, FrecencyTracker::Entry> history;
    FrecencyTracker::Entry loaded = makeEntry(2, { NOW - DAY, 0, NOW - 3 * DAY });
    loaded.frecency = 999; // whatever the server had stored
    loaded.score = 55.0;
    history.insert("thumbsup", loaded);
    tracker.overwriteHistory(history);

    REQUIRE(tracker.isDirty());
    REQUIRE(tracker.frecency("thumbsup").value() == -1);
    // Server timestamps arrive unsorted and may contain zeros.
    REQUIRE(tracker.usageHistory().value("thumbsup").recentUses ==
             QList<uint64_t>({ NOW - 3 * DAY, NOW - DAY }));

    tracker.frequently(NOW);
    // weight(3) + weight(1) = 200; ceil(2 * (200 / 2)) = 200
    REQUIRE(tracker.score("thumbsup").value() == 200.0);
    REQUIRE(tracker.frecency("thumbsup").value() == 200);

    // A second overwrite replaces the map wholesale.
    QHash<QString, FrecencyTracker::Entry> replacement;
    replacement.insert("eyes", makeEntry(1, { NOW - DAY }));
    tracker.overwriteHistory(replacement);
    REQUIRE(tracker.frequently(NOW) == QStringList({ "eyes" }));

    tracker.reset();
    REQUIRE(tracker.usageHistory().isEmpty());
    REQUIRE(tracker.frequently(NOW).isEmpty());
}
