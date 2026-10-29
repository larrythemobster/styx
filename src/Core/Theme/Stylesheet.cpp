#include "Core/Theme/Stylesheet.hpp"
#include "Core/Theme/Manager.hpp"

namespace Styx::Core::Theme {
std::string buildStyleSheet()
{
    const auto &m = Manager::instance();
    return "body { background-color: " + m.color(Token::WindowBg).css() +
        "; color: " + m.color(Token::PrimaryText).css() + "; }";
}
}
