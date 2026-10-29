#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"
#include "Platform/Native.hpp"

namespace Styx::Core::Theme::Icons {
std::string svg(std::string_view name, Platform::Color color)
{
    if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string_view::npos) return {};
    auto data = Platform::Resources::read("icons/lucide/" + std::string(name) + ".svg");
    const auto replacement = color.css();
    for (size_t pos = 0; (pos = data.find("currentColor", pos)) != std::string::npos; pos += replacement.size())
        data.replace(pos, 12, replacement);
    return data;
}
std::string svg(std::string_view name, Token token) { return svg(name, Manager::instance().color(token)); }
}
