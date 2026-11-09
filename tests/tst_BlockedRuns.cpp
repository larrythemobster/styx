#include <catch2/catch_test_macros.hpp>

#include "UI/Chat/BlockedRuns.hpp"

#include <vector>

using namespace Styx;
using namespace Styx::UI;

static void requireRun(BlockedRun run, int expectedFirst, int expectedCount)
{
    REQUIRE(run.first == expectedFirst);
    REQUIRE(run.count == expectedCount);
}

static std::vector<bool> samplePattern()
{
    return {false, true, true, false, true};
}

TEST_CASE("Pattern with grouped and isolated blocked rows", "[blocked_runs]")
{
    const auto pattern = samplePattern();
    requireRun(blockedRunAt(pattern, 1), 1, 2);
    requireRun(blockedRunAt(pattern, 2), 1, 2);
    requireRun(blockedRunAt(pattern, 4), 4, 1);
    REQUIRE(blockedRunAt(pattern, 0).count == 0);
    REQUIRE(blockedRunAt(pattern, 3).count == 0);
}

TEST_CASE("All blocked rows return entire vector as run", "[blocked_runs]")
{
    const std::vector<bool> allBlocked = {true, true, true};
    requireRun(blockedRunAt(allBlocked, 0), 0, 3);
    requireRun(blockedRunAt(allBlocked, 1), 0, 3);
    requireRun(blockedRunAt(allBlocked, 2), 0, 3);
}

TEST_CASE("Empty vector and out of range rows give zero count", "[blocked_runs]")
{
    const auto pattern = samplePattern();
    REQUIRE(blockedRunAt({}, 0).count == 0);
    REQUIRE(blockedRunAt(pattern, -1).count == 0);
    REQUIRE(blockedRunAt(pattern, static_cast<int>(pattern.size())).count == 0);
}
