#include "Core/Theme/Stylesheet.hpp"
#include "Core/Theme/Manager.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

namespace Styx::Core::Theme {
namespace {
std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

uint32_t rgb(Platform::Color color)
{
    return (uint32_t(color.r) << 16) | (uint32_t(color.g) << 8) | color.b;
}

Platform::Color contrastText(Platform::Color background)
{
    auto linear = [](uint8_t channel) {
        const double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const double luminance = .2126 * linear(background.r) + .7152 * linear(background.g) + .0722 * linear(background.b);
    return Platform::Color::rgb(luminance > .179 ? 0x000000 : 0xffffff);
}

Platform::Color blend(Platform::Color over, Platform::Color under, double amount)
{
    auto channel = [amount](uint8_t a, uint8_t b) { return uint8_t(std::lround(a * amount + b * (1 - amount))); };
    return Platform::Color{channel(over.r, under.r), channel(over.g, under.g), channel(over.b, under.b), 255, true};
}

// Stylesheets are written against Discord's default accents; these values follow the applied highlight.
std::optional<Platform::Color> accentColor(uint32_t value)
{
    const auto &manager = Manager::instance();
    auto highlight = manager.color(Token::Highlight);
    switch (value) {
    case 0x5865f2: case 0x007aa7:
        return highlight;
    case 0x4752c4: case 0x00658a: {
        const bool darkText = contrastText(highlight).r == 0;
        auto shade = [darkText](uint8_t channel) {
            return uint8_t(darkText ? channel + (255 - channel) * .12 : channel * .85);
        };
        highlight.r = shade(highlight.r);
        highlight.g = shade(highlight.g);
        highlight.b = shade(highlight.b);
        return highlight;
    }
    case 0x3c4270: case 0x0b1f29:
        return blend(highlight, manager.color(Token::AlternateBaseBg), .3);
    case 0x29b6f6: case 0x8fa5ff:
        return manager.color(Token::BrightText);
    }
    return {};
}

std::optional<Platform::Color> paletteColor(Platform::Color source, std::string_view property,
                                          std::string_view selector, std::optional<Platform::Color> buttonBackground)
{
    // Media overlays keep their dark palette independently of the application theme.
    if (selector.find("viewer-") != selector.npos || selector.find("#image-viewer") != selector.npos
        || selector.find(".call-video-frame") != selector.npos)
        return {};
    // Status dots and mention badges cut a ring out of the surface behind them.
    if (property.starts_with("border")
        && (selector.find("status-dot") != selector.npos || selector.find("mention-badge") != selector.npos))
        property = "background-color";
    auto &manager = Manager::instance();
    const auto value = rgb(source);
    std::optional<Platform::Color> result;
    switch (value) {
    case 0x000000:
        if (source.a != 255 || property.starts_with("border"))
            return {};
        result = manager.color(Token::WindowBg);
        break;
    case 0x08080a: case 0x0b0b0c:
        result = manager.color(Token::BaseBg);
        break;
    case 0x111214: case 0x1e1f22: case 0x292b2f: case 0x292b30: case 0x2b2d31: case 0x313338:
        result = manager.color(Token::AlternateBaseBg);
        break;
    case 0x19191c: case 0x202024: case 0x202124: case 0x242428: case 0x303034: case 0x303035:
    case 0x303136: case 0x35373c: case 0x404249: case 0x4e5058: case 0x6d6f78:
        result = manager.color(property.starts_with("border") ? Token::Divider : Token::ButtonBg);
        break;
    case 0x72767d: case 0x949ba4: case 0xa7a9ad:
        result = manager.color(Token::PlaceholderText);
        break;
    case 0xb5bac1: case 0xf2f3f5:
        result = manager.color(Token::WindowText);
        break;
    case 0xdbdee1:
        result = manager.color(Token::PrimaryText);
        break;
    case 0x00a8fc:
        result = manager.color(Token::LinkText);
        break;
    case 0xc9cdfb:
        result = manager.color(Token::MentionText);
        break;
    case 0xff7378: case 0xf28b82:
        result = manager.color(Token::ChatError);
        break;
    case 0xffffff:
        if (property == "image-color" && selector.find("icon") == selector.npos && selector.find("btn") == selector.npos)
            return {}; // Preserve image pixels, including profile photos.
        if (selector.find("thumb") != selector.npos || selector.find("sliderbar") != selector.npos)
            return {};
        if (buttonBackground)
            result = contrastText(*buttonBackground);
        else if (selector.find("primary") != selector.npos || selector.find("confirm") != selector.npos
                 || selector.find("badge") != selector.npos || selector.find("kind-home") != selector.npos
                 || selector.find("component-button") != selector.npos || selector.find("poll-vote") != selector.npos)
            result = contrastText(manager.color(Token::Highlight));
        else
            result = manager.color(Token::WindowText);
        break;
    default:
        result = accentColor(value);
        if (result)
            break;
        if (std::max({source.r, source.g, source.b}) - std::min({source.r, source.g, source.b}) > 16)
            return {};
        if (property.starts_with("border"))
            result = manager.color(Token::Divider);
        else if (value < 0x303030)
            result = manager.color(Token::AlternateBaseBg);
        else if (value < 0x707070)
            result = manager.color(Token::ButtonBg);
        else if (value < 0xb0b0b0)
            result = manager.color(Token::PlaceholderText);
        else
            result = manager.color(Token::WindowText);
        break;
    }
    if (source.a != 255)
        result->a = source.a;
    return result;
}

std::optional<Platform::Color> coloredBackground(std::string_view block)
{
    auto pos = block.find("background-color:");
    if (pos == block.npos)
        pos = block.find("background:");
    if (pos == block.npos)
        return {};
    const auto end = block.find(';', pos);
    const auto hash = block.find('#', pos);
    if (hash == block.npos || (end != block.npos && hash > end))
        return {};
    auto color = Platform::Color::parse(trim(block.substr(hash, end - hash)));
    if (!color.valid || std::max({color.r, color.g, color.b}) - std::min({color.r, color.g, color.b}) < 32)
        return {};
    if (auto accent = accentColor(rgb(color)))
        return accent;
    return color;
}
}

std::string buildStyleSheet(std::string source)
{
    const auto &manager = Manager::instance();
    bool colors = manager.usesSystemColors();
    for (const auto &token : registry())
        colors = colors || manager.hasOverride(token.token);
    bool fonts = false;
    for (const auto &font : fontRegistry())
        fonts = fonts || manager.hasFontOverride(font.role);
    if (!colors && !fonts)
        return source;

    std::string result;
    result.reserve(source.size());
    std::string_view text(source), selector, property;
    std::optional<Platform::Color> buttonBackground;
    size_t statement = 0, copied = 0;
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text.substr(i, 2) == "/*") {
            const auto end = text.find("*/", i + 2);
            if (end == text.npos)
                break;
            i = end + 1;
            continue;
        }
        if (text[i] == '\'' || text[i] == '"') {
            const char quote = text[i++];
            while (i < text.size() && text[i] != quote) {
                if (text[i] == '\\' && i + 1 < text.size())
                    ++i;
                ++i;
            }
            continue;
        }
        if (text[i] == '{') {
            ++depth;
            selector = trim(text.substr(statement, i - statement));
            const auto end = text.find('}', i + 1);
            buttonBackground = coloredBackground(text.substr(i + 1, end - i - 1));
            property = {};
            statement = i + 1;
        } else if (text[i] == '}') {
            --depth;
            property = {};
            statement = i + 1;
        } else if (text[i] == ';') {
            property = {};
            statement = i + 1;
        } else if (text[i] == ':' && depth && property.empty()) {
            property = trim(text.substr(statement, i - statement));
        } else if (text[i] == '#' && colors && !property.empty()) {
            size_t end = i + 1;
            while (end < text.size() && ((text[end] >= '0' && text[end] <= '9')
                   || (text[end] >= 'a' && text[end] <= 'f') || (text[end] >= 'A' && text[end] <= 'F')))
                ++end;
            const auto color = Platform::Color::parse(text.substr(i, end - i));
            if (color.valid) {
                if (auto themed = paletteColor(color, property, selector, buttonBackground)) {
                    result.append(text.substr(copied, i - copied));
                    result += themed->css();
                    copied = end;
                }
            }
            i = end - 1;
        } else if (fonts && property == "font-size" && text[i] >= '0' && text[i] <= '9') {
            double size = 0;
            const auto parsed = std::from_chars(text.data() + i, text.data() + text.size(), size);
            const size_t end = size_t(parsed.ptr - text.data());
            if (parsed.ec == std::errc{} && text.substr(end, 2) == "dp") {
                const auto role = selector.find("code") != selector.npos ? FontRole::Code
                    : selector.find(".message-body") != selector.npos ? FontRole::Message : FontRole::Ui;
                const double scale = manager.font(role).pointSize / fontDescriptor(role).defaultFont.pointSize;
                if (scale != 1) {
                    result.append(text.substr(copied, i - copied));
                    result += std::to_string(size * scale);
                    copied = end;
                }
                i = end + 1;
            }
        }
    }
    result.append(text.substr(copied));
    return result;
}
}
