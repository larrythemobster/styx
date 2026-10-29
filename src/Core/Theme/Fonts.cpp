#include "Fonts.hpp"

namespace Styx::Core::Theme {

const std::vector<FontDescriptor> &fontRegistry()
{
    static const std::vector<FontDescriptor> reg = {
        { FontRole::Ui, "font.ui", "Interface", { "Segoe UI", 10, 400, false } },
        { FontRole::Message, "font.message", "Messages", { "Segoe UI", 10, 400, false } },
        { FontRole::Code, "font.code", "Code", { "Consolas", 10, 400, false } },
    };
    return reg;
}

const FontDescriptor *findFontById(std::string_view id)
{
    for (const auto &d : fontRegistry())
        if (id == d.id) return &d;
    return nullptr;
}

const FontDescriptor &fontDescriptor(FontRole role)
{
    for (const auto &d : fontRegistry())
        if (d.role == role) return d;
    return fontRegistry().front();
}

}
