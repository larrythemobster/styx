#include <catch2/catch_test_macros.hpp>

#include "UI/Chat/SelectCandidates.hpp"

#include <string>
#include <vector>

using namespace Styx::UI;

namespace {

std::vector<std::string> candidateValues(const std::vector<SelectCandidate> &candidates)
{
    std::vector<std::string> values;
    values.reserve(candidates.size());
    for (const auto &candidate : candidates)
        values.push_back(candidate.value);
    return values;
}

} // namespace

TEST_CASE("Prefix matches precede inner matches case-insensitively", "[selects]")
{
    const std::vector<SelectCandidate> candidates{
        {"1", "Malory", "user"},
        {"2", "Alice", "user"},
        {"3", "ALBERT", "user"},
        {"4", "Bob", "user"},
    };

    const auto filtered = filterSelectCandidates(candidates, "al", 10);
    REQUIRE(candidateValues(filtered) == std::vector<std::string>{"2", "3", "1"});
}

TEST_CASE("Filter limits cut candidates and empty queries preserve leading items", "[selects]")
{
    const std::vector<SelectCandidate> candidates{
        {"1", "Malory", "user"},
        {"2", "Alice", "user"},
        {"3", "ALBERT", "user"},
        {"4", "Bob", "user"},
    };

    const auto limited = filterSelectCandidates(candidates, "al", 2);
    REQUIRE(candidateValues(limited) == std::vector<std::string>{"2", "3"});

    const auto emptyQuery = filterSelectCandidates(candidates, "", 2);
    REQUIRE(candidateValues(emptyQuery) == std::vector<std::string>{"1", "2"});
}

TEST_CASE("Non-ASCII labels match query case-insensitively", "[selects]")
{
    const std::vector<SelectCandidate> candidates{
        {"1", "Émile", "user"},
        {"2", "émilie", "user"},
        {"3", "Jean", "user"},
    };

    const auto filtered = filterSelectCandidates(candidates, "É", 5);
    REQUIRE(candidateValues(filtered) == std::vector<std::string>{"1", "2"});
}
