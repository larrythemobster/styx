#pragma once

#include <string>

#include "Core/Snowflake.hpp"

namespace Styx {
namespace Core {

struct EmojiMatch
{
    // replacement. `:name:` or `<:name:id>` / `<a:name:id>`
    std::string insertText;
    std::string displayLabel;
    std::string surrogates; // unicode only
    Snowflake customId; // custom only
    std::string imageUrl; // custom only

    [[nodiscard]] bool isCustom() const { return customId.isValid(); }
};

} // namespace Core
} // namespace Styx
