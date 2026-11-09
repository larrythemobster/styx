#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Styx::UI {

// How many options a string select accepts, after Discord's defaults (1 and 1) and
// bounds (min 0-25, max 1-25), and never more than the select offers.
struct SelectLimits
{
    int min = 1;
    int max = 1;

    [[nodiscard]] bool single() const { return max == 1; }
};

[[nodiscard]] SelectLimits selectLimits(std::optional<int> minValues, std::optional<int> maxValues, int optionCount);

// Applies a click on an option. A single select replaces the choice and returns true:
// the caller sends it at once. A multi-select toggles the value, ignoring additions past
// the maximum, and returns false.
bool toggleSelectValue(std::vector<std::string> &values, const std::string &value, SelectLimits limits);

[[nodiscard]] bool canSubmitSelect(const std::vector<std::string> &values, SelectLimits limits);

} // namespace Styx::UI
