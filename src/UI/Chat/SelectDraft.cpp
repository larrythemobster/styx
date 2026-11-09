#include "UI/Chat/SelectDraft.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace Styx::UI {

SelectLimits selectLimits(std::optional<int> minValues, std::optional<int> maxValues, int optionCount)
{
    int max = std::clamp(maxValues.value_or(1), 1, 25);
    max = std::min(max, std::max(optionCount, 1));
    int min = std::clamp(minValues.value_or(1), 0, 25);
    min = std::min(min, max);
    return {min, max};
}

bool toggleSelectValue(std::vector<std::string> &values, const std::string &value, SelectLimits limits)
{
    if (limits.single()) {
        values.assign(1, value);
        return true;
    }

    const auto it = std::ranges::find(values, value);
    if (it != values.end())
        values.erase(it);
    else if (static_cast<int>(values.size()) < limits.max)
        values.push_back(value);
    return false;
}

bool canSubmitSelect(const std::vector<std::string> &values, SelectLimits limits)
{
    const auto count = static_cast<int>(values.size());
    return count >= limits.min && count <= limits.max;
}

} // namespace Styx::UI
