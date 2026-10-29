#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Styx {
namespace Core {
namespace Theme {

enum class FontRole {
    Ui,
    Message,
    Code,
};

struct Font {
    std::string family;
    double pointSize = 10;
    int weight = 400;
    bool italic = false;
};

struct FontDescriptor
{
    FontRole role;
    const char *id;
    const char *label;
    Font defaultFont;
};

const std::vector<FontDescriptor> &fontRegistry();
const FontDescriptor *findFontById(std::string_view id);
const FontDescriptor &fontDescriptor(FontRole role);


} // namespace Theme
} // namespace Core
} // namespace Styx
