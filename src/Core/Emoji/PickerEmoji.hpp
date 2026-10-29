#pragma once

#include <string>
#include <vector>

#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

namespace Styx {
namespace Core {

enum class EmojiIntention {
    Reaction,
    Chat,
};

struct PickerEmoji
{
    std::string name;
    std::string surrogates;
    Snowflake customId;
    std::string customName;
    Snowflake guildId;
    bool animated = false;

    [[nodiscard]] bool isCustom() const { return customId.isValid(); }

    [[nodiscard]] bool sameEmoji(const PickerEmoji &other) const
    {
        return isCustom() ? customId == other.customId : (!other.isCustom() && surrogates == other.surrogates);
    }

    [[nodiscard]] Discord::Emoji toReactionEmoji() const
    {
        return isCustom() ? Discord::Emoji::custom(customId, customName, animated) : Discord::Emoji::unicode(surrogates);
    }

    [[nodiscard]] std::string messageText() const
    {
        if (!isCustom())
            return ":" + name + ":";
        return (animated ? "<a:" : "<:") + customName + ":" + customId.toString() + ">";
    }
};

struct PickerSection
{
    static constexpr auto RecentId = "recent";

    std::string id;
    std::string title;
    Snowflake guildId;
    std::string guildIconUrl;
    std::vector<PickerEmoji> emojis;
};

} // namespace Core
} // namespace Styx
