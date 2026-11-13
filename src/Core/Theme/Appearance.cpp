#include "Core/Theme/Appearance.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "Core/SettingsKeys.hpp"
#include "Core/Theme/Manager.hpp"
#include "Platform/Settings.hpp"

namespace Styx::Core::Theme {
namespace {
constexpr std::array<std::pair<ThemeChoice, std::string_view>, 5> choiceIds{{
    {ThemeChoice::Light, "light"}, {ThemeChoice::Ash, "ash"}, {ThemeChoice::Dark, "dark"},
    {ThemeChoice::Onyx, "onyx"}, {ThemeChoice::Custom, "custom"}}};

struct PresetPalette
{
    uint32_t window, base, alternate, button, divider, windowText, primaryText, placeholder, disabled,
        bright, link, error, mentionText, mentionBackground;
};

std::unordered_map<Token, Platform::Color> presetColors(const PresetPalette &p)
{
    auto rgb = [](uint32_t value) { return Platform::Color::rgb(value); };
    auto mention = Platform::Color::rgb(p.mentionBackground >> 8);
    mention.a = uint8_t(p.mentionBackground & 0xff);
    return {
        {Token::WindowBg, rgb(p.window)}, {Token::BaseBg, rgb(p.base)}, {Token::AlternateBaseBg, rgb(p.alternate)},
        {Token::ButtonBg, rgb(p.button)}, {Token::TooltipBg, rgb(p.button)},
        {Token::WindowText, rgb(p.windowText)}, {Token::PrimaryText, rgb(p.primaryText)},
        {Token::ButtonText, rgb(p.windowText)}, {Token::TooltipText, rgb(p.primaryText)},
        {Token::PlaceholderText, rgb(p.placeholder)}, {Token::DisabledText, rgb(p.disabled)},
        {Token::BrightText, rgb(p.bright)}, {Token::Highlight, rgb(0x5865f2)}, {Token::HighlightedText, rgb(0xffffff)},
        {Token::LinkText, rgb(p.link)}, {Token::Divider, rgb(p.divider)}, {Token::ChatError, rgb(p.error)},
        {Token::MentionText, rgb(p.mentionText)}, {Token::MentionBg, mention},
    };
}

std::unordered_map<Token, Platform::Color> presetColors(ThemeChoice choice)
{
    switch (choice) {
    case ThemeChoice::Light:
        return presetColors({0xffffff, 0xf7f7f8, 0xf0f1f3, 0xe3e5e8, 0xd4d6db, 0x060607, 0x313338, 0x5c5e66, 0x9b9da5,
                             0x4752c4, 0x006ce7, 0xda373c, 0x4752c4, 0x5865f226});
    case ThemeChoice::Ash:
        return presetColors({0x323339, 0x37383f, 0x3c3d44, 0x47484f, 0x4a4b52, 0xf2f3f5, 0xdbdee1, 0x9b9ca3, 0x6d6f78,
                             0x949cf7, 0x00a8fc, 0xf23f43, 0xc9cdfb, 0x5865f24d});
    case ThemeChoice::Dark:
        return presetColors({0x1a1a1e, 0x1f1f23, 0x242429, 0x2e2e34, 0x2f2f35, 0xf2f3f5, 0xdbdee1, 0x949ba4, 0x6d6f78,
                             0x949cf7, 0x00a8fc, 0xf23f43, 0xc9cdfb, 0x5865f24d});
    case ThemeChoice::Onyx:
    case ThemeChoice::Custom:
        break;
    }
    return {};
}
}

std::string_view choiceId(ThemeChoice choice)
{
    for (const auto &[value, id] : choiceIds)
        if (value == choice)
            return id;
    return "onyx";
}

std::optional<ThemeChoice> choiceFromId(std::string_view id)
{
    for (const auto &[value, name] : choiceIds)
        if (name == id)
            return value;
    return {};
}

std::optional<ThemeChoice> choiceFromProto(uint32_t theme)
{
    switch (theme) {
    case 0: case 1: return ThemeChoice::Ash;
    case 2: return ThemeChoice::Light;
    case 3: return ThemeChoice::Dark;
    case 4: return ThemeChoice::Onyx;
    default: return {};
    }
}

std::optional<uint32_t> protoFromChoice(ThemeChoice choice)
{
    switch (choice) {
    case ThemeChoice::Ash: return 1;
    case ThemeChoice::Light: return 2;
    case ThemeChoice::Dark: return 3;
    case ThemeChoice::Onyx: return 4;
    case ThemeChoice::Custom: break;
    }
    return {};
}

ThemeChoice effectiveChoice(const Appearance &appearance, DeviceTheme device)
{
    if (!appearance.followDevice || device == DeviceTheme::Unknown || appearance.choice == ThemeChoice::Custom)
        return appearance.choice;
    if (device == DeviceTheme::Light)
        return ThemeChoice::Light;
    return appearance.choice == ThemeChoice::Light ? ThemeChoice::Dark : appearance.choice;
}

bool effectiveCustomDark(const Appearance &appearance, DeviceTheme device)
{
    if (!appearance.followDevice || device == DeviceTheme::Unknown)
        return appearance.customDark;
    return device == DeviceTheme::Dark;
}

std::unordered_map<Token, Platform::Color> appearancePalette(const Appearance &appearance, DeviceTheme device)
{
    const auto choice = effectiveChoice(appearance, device);
    if (choice == ThemeChoice::Custom)
        return generate(appearance.seed, appearance.scheme, effectiveCustomDark(appearance, device));
    return presetColors(choice);
}

Appearance loadAppearance()
{
    namespace Keys = Settings::Keys;
    const auto &settings = Platform::Settings::instance();
    const auto &manager = Manager::instance();
    Appearance appearance;
    // Before default themes existed, any saved palette came from the custom generator.
    bool customized = false;
    for (const auto &token : registry())
        customized = customized || manager.hasOverride(token.token);
    appearance.choice = choiceFromId(settings.getString(Keys::ThemeChoice))
        .value_or(customized ? ThemeChoice::Custom : ThemeChoice::Onyx);
    appearance.followDevice = settings.getBool(Keys::ThemeFollowDevice, Settings::Defaults::ThemeFollowDevice);
    appearance.syncTheme = settings.getBool(Keys::ThemeSync, Settings::Defaults::ThemeSync);
    const auto highlight = manager.color(Token::Highlight);
    appearance.seed = Platform::Color::parse(settings.getString(Keys::ThemeSeedColor, highlight.css()));
    if (!appearance.seed.valid)
        appearance.seed = highlight;
    appearance.scheme = static_cast<Scheme>(std::clamp(settings.getInt(Keys::ThemeScheme, 0), 0, schemeCount - 1));
    appearance.customDark = settings.getBool(Keys::ThemeDarkMode, true);
    return appearance;
}

bool storeAppearance(const Appearance &appearance)
{
    namespace Keys = Settings::Keys;
    auto &settings = Platform::Settings::instance();
    settings.setString(Keys::ThemeChoice, choiceId(appearance.choice));
    settings.setBool(Keys::ThemeFollowDevice, appearance.followDevice);
    settings.setBool(Keys::ThemeSync, appearance.syncTheme);
    settings.setString(Keys::ThemeSeedColor, appearance.seed.css());
    settings.setInt(Keys::ThemeScheme, static_cast<int>(appearance.scheme));
    settings.setBool(Keys::ThemeDarkMode, appearance.customDark);
    return settings.save();
}

bool applyAppearance(const Appearance &appearance, DeviceTheme device)
{
    auto &manager = Manager::instance();
    manager.setOverrides(appearancePalette(appearance, device));
    manager.apply();
    return manager.save();
}

void restoreAppearance(DeviceTheme device)
{
    auto &manager = Manager::instance();
    if (Platform::Settings::instance().contains(Settings::Keys::ThemeChoice))
        manager.setOverrides(appearancePalette(loadAppearance(), device));
    manager.apply();
}

} // namespace Styx::Core::Theme
