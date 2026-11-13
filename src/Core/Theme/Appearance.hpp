#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "Core/Theme/Generator.hpp"
#include "Core/Theme/Tokens.hpp"

namespace Styx::Core::Theme {

// Discord's default themes, plus the theme generated locally from an accent color.
enum class ThemeChoice { Light, Ash, Dark, Onyx, Custom };
enum class DeviceTheme { Unknown, Light, Dark };

// Device-wide appearance preferences. Precedence when building the palette:
// 1. Following the device decides light versus dark whenever the system reports one.
//    A light device shows Light (or the custom theme in light mode); a dark device keeps
//    the chosen dark theme, uses Dark when Light was chosen, and the custom theme in dark mode.
// 2. Otherwise the chosen theme applies as is. With theme sync on, the chosen default theme
//    mirrors the current Discord account's theme; the custom theme is never synced.
// Windows system colors are a separate override applied by Manager::color.
struct Appearance
{
    ThemeChoice choice = ThemeChoice::Onyx;
    bool followDevice = false;
    bool syncTheme = true;
    Platform::Color seed;
    Scheme scheme = Scheme::Monochromatic;
    bool customDark = true;
};

std::string_view choiceId(ThemeChoice choice);
std::optional<ThemeChoice> choiceFromId(std::string_view id);
// Settings-proto theme values: UNSET and DARK are Ash, LIGHT, DARKER is Dark, MIDNIGHT is Onyx.
std::optional<ThemeChoice> choiceFromProto(uint32_t theme);
std::optional<uint32_t> protoFromChoice(ThemeChoice choice);

ThemeChoice effectiveChoice(const Appearance &appearance, DeviceTheme device);
bool effectiveCustomDark(const Appearance &appearance, DeviceTheme device);
// Token overrides for the effective theme; Onyx is the stylesheets' own palette and needs none.
std::unordered_map<Token, Platform::Color> appearancePalette(const Appearance &appearance, DeviceTheme device);

Appearance loadAppearance();
// Stores the preferences and writes the settings file; returns whether it was written.
bool storeAppearance(const Appearance &appearance);
// Applies the palette to open and future documents; returns whether theme.json was saved.
bool applyAppearance(const Appearance &appearance, DeviceTheme device);
// Startup: rebuilds the palette from stored preferences for the current device theme.
// Without stored preferences the palette loaded from theme.json stays in place.
void restoreAppearance(DeviceTheme device);

} // namespace Styx::Core::Theme
