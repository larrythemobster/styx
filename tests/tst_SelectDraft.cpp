#include <catch2/catch_test_macros.hpp>

#include "UI/Chat/SelectDraft.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace Styx::UI;

TEST_CASE("Select limits clamp to valid ranges and option counts", "[selects]")
{
    const auto defaultLimits = selectLimits(std::nullopt, std::nullopt, 5);
    REQUIRE(defaultLimits.min == 1);
    REQUIRE(defaultLimits.max == 1);

    const auto customLimits = selectLimits(0, 3, 5);
    REQUIRE(customLimits.min == 0);
    REQUIRE(customLimits.max == 3);

    const auto cappedToOptionCount = selectLimits(std::nullopt, 10, 2);
    REQUIRE(cappedToOptionCount.max == 2);

    const auto minClampedToMax = selectLimits(5, 2, 5);
    REQUIRE(minClampedToMax.min == 2);
    REQUIRE(minClampedToMax.max == 2);

    const auto outOfRange = selectLimits(-3, 40, 30);
    REQUIRE(outOfRange.min == 0);
    REQUIRE(outOfRange.max == 25);
}

TEST_CASE("Single select replaces choice and requests immediate send", "[selects]")
{
    const SelectLimits limits{1, 1};
    std::vector<std::string> values;

    REQUIRE(toggleSelectValue(values, "first", limits));
    REQUIRE(values == std::vector<std::string>{"first"});

    REQUIRE(toggleSelectValue(values, "second", limits));
    REQUIRE(values == std::vector<std::string>{"second"});
}

TEST_CASE("Multi select toggles values and enforces maximum", "[selects]")
{
    const SelectLimits limits{1, 2};
    std::vector<std::string> values;

    REQUIRE_FALSE(toggleSelectValue(values, "a", limits));
    REQUIRE(values == std::vector<std::string>{"a"});

    REQUIRE_FALSE(toggleSelectValue(values, "b", limits));
    REQUIRE(values == std::vector<std::string>{"a", "b"});

    REQUIRE_FALSE(toggleSelectValue(values, "c", limits));
    REQUIRE(values == std::vector<std::string>{"a", "b"});

    REQUIRE_FALSE(toggleSelectValue(values, "a", limits));
    REQUIRE(values == std::vector<std::string>{"b"});
}

TEST_CASE("Can submit verifies selection size bounds", "[selects]")
{
    const SelectLimits limits{2, 3};
    std::vector<std::string> values;

    REQUIRE_FALSE(canSubmitSelect(values, limits));

    values.push_back("a");
    REQUIRE_FALSE(canSubmitSelect(values, limits));

    values.push_back("b");
    REQUIRE(canSubmitSelect(values, limits));

    values.push_back("c");
    REQUIRE(canSubmitSelect(values, limits));

    values.push_back("d");
    REQUIRE_FALSE(canSubmitSelect(values, limits));

    const SelectLimits optionalLimits{0, 2};
    REQUIRE(canSubmitSelect({}, optionalLimits));
}
