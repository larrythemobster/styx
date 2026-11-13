#include "Core/Theme/Scale.hpp"

#include <algorithm>
#include <cmath>

#include "Core/SettingsKeys.hpp"
#include "Core/Theme/Manager.hpp"
#include "Platform/Settings.hpp"

namespace Styx::Core::Theme {

int nearestStop(std::span<const int> stops, double value)
{
    int best = stops.front();
    for (const int stop : stops)
        if (std::abs(stop - value) < std::abs(best - value))
            best = stop;
    return best;
}

int stepStop(std::span<const int> stops, int current, int direction)
{
    const auto at = std::find(stops.begin(), stops.end(), nearestStop(stops, current));
    if (direction > 0)
        return at + 1 == stops.end() ? *at : *(at + 1);
    return at == stops.begin() ? *at : *(at - 1);
}

int savedZoom()
{
    return nearestStop(zoomLevels, Platform::Settings::instance().getInt(Settings::Keys::InterfaceZoom, defaultZoom));
}

bool saveZoom(int percent)
{
    auto &settings = Platform::Settings::instance();
    settings.setInt(Settings::Keys::InterfaceZoom, nearestStop(zoomLevels, percent));
    return settings.save();
}

int chatTextSize()
{
    const double defaultPoints = fontDescriptor(FontRole::Message).defaultFont.pointSize;
    return nearestStop(chatTextSizes, Manager::instance().font(FontRole::Message).pointSize * defaultChatTextSize / defaultPoints);
}

void setChatTextSize(int pixels)
{
    auto &manager = Manager::instance();
    const int size = nearestStop(chatTextSizes, pixels);
    if (size == defaultChatTextSize) {
        manager.clearFontOverride(FontRole::Message);
        return;
    }
    auto font = manager.font(FontRole::Message);
    font.pointSize = size * fontDescriptor(FontRole::Message).defaultFont.pointSize / defaultChatTextSize;
    manager.setFontOverride(FontRole::Message, font);
}

bool migrateFontScaling()
{
    auto &manager = Manager::instance();
    const bool interface = manager.hasFontOverride(FontRole::Ui);
    const bool code = manager.hasFontOverride(FontRole::Code);
    if (!interface && !code)
        return false;
    auto &settings = Platform::Settings::instance();
    if (interface && !settings.contains(Settings::Keys::InterfaceZoom)) {
        const double ratio = manager.font(FontRole::Ui).pointSize / fontDescriptor(FontRole::Ui).defaultFont.pointSize;
        settings.setInt(Settings::Keys::InterfaceZoom, nearestStop(zoomLevels, ratio * 100));
    }
    manager.clearFontOverride(FontRole::Ui);
    manager.clearFontOverride(FontRole::Code);
    return true;
}

} // namespace Styx::Core::Theme
