#pragma once

#include <charconv>
#include <optional>
#include <regex>
#include <string>

#include "Core/Snowflake.hpp"

namespace Styx::Discord {

// https://[canary.|ptb.]discord[app].com/channels/<guild or @me>/<channel>[/<message>]
struct ChannelLink
{
    Core::Snowflake guildId = Core::Snowflake::Invalid;
    Core::Snowflake channelId;
    Core::Snowflake messageId = Core::Snowflake::Invalid;

    static std::regex pattern(const std::string &suffix = {})
    {
        return std::regex(R"(^https?://(?:(?:canary|ptb)\.)?discord(?:app)?\.com/channels/(@me|\d+)/(\d+)(?:/(\d+))?/?)" + suffix,
                          std::regex::icase);
    }

    static ChannelLink fromMatch(const std::smatch &match)
    {
        auto id = [](const std::ssub_match &part) {
            std::uint64_t value = 0;
            const auto text = part.str();
            std::from_chars(text.data(), text.data() + text.size(), value);
            return Core::Snowflake(value);
        };
        ChannelLink link;
        if (match[1].str() != "@me")
            link.guildId = id(match[1]);
        link.channelId = id(match[2]);
        if (match[3].matched)
            link.messageId = id(match[3]);
        return link;
    }

    static std::optional<ChannelLink> parse(const std::string &url)
    {
        static const std::regex whole = pattern("$");
        std::smatch match;
        if (!std::regex_search(url, match, whole))
            return std::nullopt;
        return fromMatch(match);
    }

    std::string toUrl() const
    {
        std::string url = "https://discord.com/channels/";
        url += guildId.isValid() ? guildId.toString() : "@me";
        url += "/" + channelId.toString();
        if (messageId.isValid())
            url += "/" + messageId.toString();
        return url;
    }
};

} // namespace Styx::Discord
