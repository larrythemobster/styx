#pragma once

#include <string>
#include <unordered_map>

#include "Core/Theme/Tokens.hpp"

namespace Styx {
namespace Core {
namespace Theme {

enum class Scheme {
    Monochromatic,
    Analogous,
    Complementary,
    SplitComplementary,
    Triadic,
};

inline constexpr int schemeCount = 5;

std::string schemeName(Scheme scheme);
std::unordered_map<Token, Platform::Color> generate(const Platform::Color &seed, Scheme scheme, bool dark);

} // namespace Theme
} // namespace Core
} // namespace Styx
