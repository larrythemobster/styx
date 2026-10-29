#pragma once

#include <filesystem>
#include <optional>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "Core/Theme/Fonts.hpp"
#include "Core/Theme/Tokens.hpp"
#include "Platform/Signal.hpp"

namespace Styx::Core::Theme {

class Manager {
public:
    static Manager &instance();
    Platform::Color color(Token token) const;
    Font font(FontRole role) const;
    bool hasOverride(Token token) const;
    void setOverride(Token token, const Platform::Color &color);
    void clearOverride(Token token);
    void resetAll();
    void setOverrides(const std::unordered_map<Token, Platform::Color> &overrides);
    bool usesSystemColors() const;
    void setUseSystemColors(bool enabled);
    bool isSystemControlled(Token token) const;
    static bool systemStyleEnabled();
    static void setSystemStyleEnabled(bool enabled);
    static bool startsWithSystemStyle();
    bool hasFontOverride(FontRole role) const;
    void setFontOverride(FontRole role, const Font &font);
    void clearFontOverride(FontRole role);
    void apply();
    void applyFonts();
    bool load();
    bool save() const;
    bool exportTo(const std::filesystem::path &path) const;
    bool importFrom(const std::filesystem::path &path);
    Platform::Signal<> themeChanged;
    Platform::Signal<> metricsChanged;
private:
    Manager() = default;
    static std::filesystem::path defaultThemePath();
    nlohmann::json toObject(bool includeDefaults) const;
    void loadFromObject(const nlohmann::json &obj);
    Platform::Color customColor(Token token) const;
    std::optional<Platform::Color> systemColor(Token token) const;
    std::unordered_map<Token, Platform::Color> overrides;
    std::unordered_map<FontRole, Font> fontOverrides;
    bool systemColors = false;
};

}
