#include "Core/Theme/Manager.hpp"
#include "Core/SettingsKeys.hpp"
#include "Platform/Paths.hpp"
#include "Platform/Settings.hpp"
#include <cstdio>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Styx::Core::Theme {
namespace {
constexpr const char *UseSystemColorsKey = Settings::Keys::ThemeUseSystemColors;
std::string hex(Platform::Color c) {
    char buf[10];
    if (c.a == 255) std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    else std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", c.a, c.r, c.g, c.b);
    return buf;
}
Font parseFont(const std::string &value, Font fallback) {
    std::istringstream in(value);
    std::string part;
    if (!std::getline(in, fallback.family, ',')) return fallback;
    if (std::getline(in, part, ',')) { try { fallback.pointSize = std::stod(part); } catch (...) {} }
    for (int i = 0; i < 3 && std::getline(in, part, ','); ++i) {
        if (i == 2) { try { int weight = std::stoi(part); fallback.weight = weight <= 99 ? weight * 10 : weight; } catch (...) {} }
    }
    if (std::getline(in, part, ',')) fallback.italic = part == "1";
    return fallback;
}
std::string serializeFont(const Font &f) {
    return f.family + "," + std::to_string(f.pointSize) + ",-1,5," + std::to_string(f.weight) + "," + (f.italic ? "1" : "0") + ",0,0,0,0";
}
}
Manager &Manager::instance() { static Manager manager; return manager; }
Platform::Color Manager::customColor(Token token) const { auto it = overrides.find(token); return it == overrides.end() ? descriptor(token).defaultColor : it->second; }
Platform::Color Manager::color(Token token) const { if (systemColors) if (auto c = systemColor(token)) return *c; return customColor(token); }
Font Manager::font(FontRole role) const { auto it = fontOverrides.find(role); return it == fontOverrides.end() ? fontDescriptor(role).defaultFont : it->second; }
bool Manager::hasOverride(Token token) const { return overrides.contains(token); }
void Manager::setOverride(Token token, const Platform::Color &color) { if (color.valid) overrides[token] = color; }
void Manager::clearOverride(Token token) { overrides.erase(token); }
void Manager::resetAll() { overrides.clear(); fontOverrides.clear(); }
void Manager::setOverrides(const std::unordered_map<Token, Platform::Color> &values) { overrides = values; }
bool Manager::usesSystemColors() const { return systemColors; }
void Manager::setUseSystemColors(bool enabled) { systemColors = enabled; Platform::Settings::instance().setBool(UseSystemColorsKey, enabled); }
bool Manager::isSystemControlled(Token token) const { return systemColors && (descriptor(token).role.has_value() || token == Token::DisabledText); }
std::optional<Platform::Color> Manager::systemColor(Token token) const {
#ifdef _WIN32
    int index = -1;
    switch (token) {
    case Token::WindowBg: index = COLOR_WINDOW; break;
    case Token::BaseBg: case Token::AlternateBaseBg: index = COLOR_WINDOW; break;
    case Token::ButtonBg: index = COLOR_BTNFACE; break;
    case Token::TooltipBg: index = COLOR_INFOBK; break;
    case Token::WindowText: case Token::PrimaryText: index = COLOR_WINDOWTEXT; break;
    case Token::ButtonText: index = COLOR_BTNTEXT; break;
    case Token::TooltipText: index = COLOR_INFOTEXT; break;
    case Token::PlaceholderText: case Token::DisabledText: index = COLOR_GRAYTEXT; break;
    case Token::BrightText: case Token::LinkText: index = COLOR_HOTLIGHT; break;
    case Token::Highlight: index = COLOR_HIGHLIGHT; break;
    case Token::HighlightedText: index = COLOR_HIGHLIGHTTEXT; break;
    case Token::Divider: index = COLOR_3DSHADOW; break;
    case Token::MentionText: case Token::MentionBg: if (!overrides.contains(token)) index = COLOR_HOTLIGHT; break;
    default: break;
    }
    if (index >= 0) { auto c = GetSysColor(index); return Platform::Color{GetRValue(c), GetGValue(c), GetBValue(c), token == Token::MentionBg ? descriptor(token).defaultColor.a : uint8_t{255}, true}; }
#endif
    return std::nullopt;
}
bool Manager::hasFontOverride(FontRole role) const { return fontOverrides.contains(role); }
void Manager::setFontOverride(FontRole role, const Font &value) { fontOverrides[role] = value; }
void Manager::clearFontOverride(FontRole role) { fontOverrides.erase(role); }
void Manager::apply() { themeChanged.publish(); }
void Manager::applyFonts() { metricsChanged.publish(); }
std::filesystem::path Manager::defaultThemePath() { auto dir = Platform::Paths::dataDir(); Platform::Paths::ensureDirExists(dir); return dir / "theme.json"; }
nlohmann::json Manager::toObject(bool defaults) const {
    nlohmann::json obj = {{"_version", 1}};
    for (const auto &d : registry()) if (defaults || overrides.contains(d.token)) obj[d.id] = hex(customColor(d.token));
    for (const auto &d : fontRegistry()) if (defaults || fontOverrides.contains(d.role)) obj[d.id] = serializeFont(font(d.role));
    return obj;
}
void Manager::loadFromObject(const nlohmann::json &obj) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.key().empty() || it.key()[0] == '_' || !it.value().is_string()) continue;
        auto value = it.value().get<std::string>();
        if (auto d = findById(it.key())) {
            if (value.size() == 9 && value[0] == '#')
                value = "#" + value.substr(3) + value.substr(1, 2);
            auto c = Platform::Color::parse(value);
            if (c.valid) overrides[d->token] = c;
        }
        else if (auto d = findFontById(it.key())) fontOverrides[d->role] = parseFont(value, d->defaultFont);
    }
}
bool Manager::load() {
    overrides.clear(); fontOverrides.clear(); systemColors = Platform::Settings::instance().getBool(UseSystemColorsKey, Settings::Defaults::ThemeUseSystemColors);
    const auto path = defaultThemePath();
    if (!std::filesystem::exists(path)) return false;
    auto data = Platform::Resources::readFile(path);
    auto obj = nlohmann::json::parse(data, nullptr, false);
    if (!obj.is_object()) return false;
    loadFromObject(obj); return true;
}
bool Manager::save() const { return Platform::Resources::writeFile(defaultThemePath(), toObject(false).dump(4)); }
bool Manager::exportTo(const std::filesystem::path &path) const { return Platform::Resources::writeFile(path, toObject(true).dump(4)); }
bool Manager::importFrom(const std::filesystem::path &path) {
    auto obj = nlohmann::json::parse(Platform::Resources::readFile(path), nullptr, false);
    if (!obj.is_object()) return false;
    overrides.clear(); fontOverrides.clear(); loadFromObject(obj); return true;
}
}
