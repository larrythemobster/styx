#include "UI/Chat/SelectCandidates.hpp"

#include "Platform/Utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace Styx::UI {

std::vector<SelectCandidate> filterSelectCandidates(const std::vector<SelectCandidate> &candidates,
                                                    std::string_view query, std::size_t limit)
{
    if (limit == 0)
        return {};

    const auto foldedQuery = Platform::caseFold(query);
    if (foldedQuery.empty()) {
        const auto count = std::min(candidates.size(), limit);
        std::vector<SelectCandidate> result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            result.push_back(candidates[i]);
        return result;
    }

    std::vector<SelectCandidate> result;
    result.reserve(std::min(candidates.size(), limit));

    std::vector<std::string> foldedLabels;
    foldedLabels.reserve(candidates.size());

    for (const auto &candidate : candidates) {
        foldedLabels.push_back(Platform::caseFold(candidate.label));
        if (foldedLabels.back().starts_with(foldedQuery)) {
            result.push_back(candidate);
            if (result.size() == limit)
                return result;
        }
    }

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto pos = foldedLabels[i].find(foldedQuery);
        if (pos != std::string::npos && pos != 0) {
            result.push_back(candidates[i]);
            if (result.size() == limit)
                return result;
        }
    }

    return result;
}

} // namespace Styx::UI
