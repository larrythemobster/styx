#include "Core/Theme/Generator.hpp"

#include "Core/Theme/Tokens.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Styx {
namespace Core {
namespace Theme {

namespace {

Platform::Color hsl(double hueDeg, double s, double l)
{
    double h = std::fmod(hueDeg, 360.0);
    if (h < 0.0)
        h += 360.0;
    s = std::clamp(s, 0.0, 1.0);
    l = std::clamp(l, 0.0, 1.0);
    double c = (1.0 - std::abs(2.0 * l - 1.0)) * s;
    double x = c * (1.0 - std::abs(std::fmod(h / 60.0, 2.0) - 1.0));
    double m = l - c / 2.0;
    double r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; } else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    return { static_cast<uint8_t>(std::lround((r + m) * 255)), static_cast<uint8_t>(std::lround((g + m) * 255)), static_cast<uint8_t>(std::lround((b + m) * 255)), 255, true };
}

struct AccentHues
{
    double primary;
    double secondary;
};

AccentHues accentHuesFor(Scheme scheme, double seedHue)
{
    switch (scheme) {
    case Scheme::Monochromatic:
        return { seedHue, seedHue };
    case Scheme::Analogous:
        return { seedHue + 30.0, seedHue - 30.0 };
    case Scheme::Complementary:
        return { seedHue + 180.0, seedHue + 180.0 };
    case Scheme::SplitComplementary:
        return { seedHue + 150.0, seedHue + 210.0 };
    case Scheme::Triadic:
        return { seedHue + 120.0, seedHue + 240.0 };
    }
    return { seedHue, seedHue };
}

} // namespace

std::string schemeName(Scheme scheme)
{
    switch (scheme) {
    case Scheme::Monochromatic:
        return "Monochromatic";
    case Scheme::Analogous:
        return "Analogous";
    case Scheme::Complementary:
        return "Complementary";
    case Scheme::SplitComplementary:
        return "Split-complementary";
    case Scheme::Triadic:
        return "Triadic";
    }
    return {};
}

std::unordered_map<Token, Platform::Color> generate(const Platform::Color &seed, Scheme scheme, bool dark)
{
    const double r = seed.r / 255.0, g = seed.g / 255.0, b = seed.b / 255.0;
    const double hi = std::max({r, g, b}), lo = std::min({r, g, b});
    const double delta = hi - lo, lightness = (hi + lo) / 2.0;
    const double seedSat = delta == 0 ? 0 : delta / (1.0 - std::abs(2.0 * lightness - 1.0));
    double seedHue = 250.0;
    if (delta != 0) {
        seedHue = 60.0 * (hi == r ? std::fmod((g - b) / delta, 6.0) : hi == g ? (b - r) / delta + 2.0 : (r - g) / delta + 4.0);
        if (seedHue < 0) seedHue += 360.0;
    }
    const AccentHues accents = accentHuesFor(scheme, seedHue);
    const double accentHue = accents.primary;
    const double linkHue = accents.secondary;

    const double bgSat = std::clamp(seedSat * 0.5, 0.10, 0.45);
    const double textSat = std::clamp(seedSat * 0.35, 0.04, 0.22);
    const double accSat = std::clamp(std::max(seedSat, 0.55), 0.5, 0.95);

    std::unordered_map<Token, Platform::Color> m;

    if (dark) {
        m.emplace(Token::WindowBg, hsl(seedHue, bgSat, 0.085));
        m.emplace(Token::BaseBg, hsl(seedHue, bgSat, 0.12));
        m.emplace(Token::AlternateBaseBg, hsl(seedHue, bgSat, 0.16));
        m.emplace(Token::ButtonBg, hsl(seedHue, bgSat, 0.19));
        m.emplace(Token::TooltipBg, hsl(seedHue, bgSat, 0.19));

        m.emplace(Token::WindowText, hsl(seedHue, textSat, 0.88));
        m.emplace(Token::PrimaryText, hsl(seedHue, textSat, 0.84));
        m.emplace(Token::ButtonText, hsl(seedHue, textSat, 0.88));
        m.emplace(Token::TooltipText, hsl(seedHue, textSat, 0.92));
        m.emplace(Token::PlaceholderText, hsl(seedHue, textSat, 0.55));
        m.emplace(Token::DisabledText, hsl(seedHue, textSat, 0.45));
        m.emplace(Token::BrightText, hsl(accentHue, accSat, 0.75));

        m.emplace(Token::Highlight, hsl(accentHue, accSat, 0.60));
        m.emplace(Token::HighlightedText, hsl(accentHue, 0.25, 0.97));
        m.emplace(Token::LinkText, hsl(linkHue, 0.70, 0.72));
        m.emplace(Token::Divider, hsl(seedHue, bgSat, 0.30));

        m.emplace(Token::MentionText, hsl(accentHue, 0.65, 0.82));
        Platform::Color mentionBg = hsl(accentHue, accSat, 0.58);
        mentionBg.a = 77; // ~30%
        m.emplace(Token::MentionBg, mentionBg);
    } else {
        m.emplace(Token::WindowBg, hsl(seedHue, bgSat, 0.97));
        m.emplace(Token::BaseBg, hsl(seedHue, bgSat, 0.99));
        m.emplace(Token::AlternateBaseBg, hsl(seedHue, bgSat, 0.93));
        m.emplace(Token::ButtonBg, hsl(seedHue, bgSat, 0.90));
        m.emplace(Token::TooltipBg, hsl(seedHue, bgSat, 0.92));

        m.emplace(Token::WindowText, hsl(seedHue, textSat, 0.18));
        m.emplace(Token::PrimaryText, hsl(seedHue, textSat, 0.22));
        m.emplace(Token::ButtonText, hsl(seedHue, textSat, 0.18));
        m.emplace(Token::TooltipText, hsl(seedHue, textSat, 0.16));
        m.emplace(Token::PlaceholderText, hsl(seedHue, textSat, 0.40));
        m.emplace(Token::DisabledText, hsl(seedHue, textSat, 0.60));
        m.emplace(Token::BrightText, hsl(accentHue, accSat, 0.40));

        m.emplace(Token::Highlight, hsl(accentHue, accSat, 0.55));
        m.emplace(Token::HighlightedText, hsl(accentHue, 0.20, 0.99));
        m.emplace(Token::LinkText, hsl(linkHue, 0.75, 0.30));
        m.emplace(Token::Divider, hsl(seedHue, bgSat, 0.78));

        m.emplace(Token::MentionText, hsl(accentHue, 0.70, 0.28));
        Platform::Color mentionBg = hsl(accentHue, accSat, 0.55);
        mentionBg.a = 64; // ~25%
        m.emplace(Token::MentionBg, mentionBg);
    }

    m.emplace(Token::ChatError, hsl(2.0, 0.70, dark ? 0.62 : 0.50));
    m.emplace(Token::EmbedDefault, m.at(Token::Highlight));

    return m;
}

} // namespace Theme
} // namespace Core
} // namespace Styx
