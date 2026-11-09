#include "InviteLink.hpp"
#include "Core/JsonUtils.hpp"
#include "Platform/Utf8.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Styx::Discord {

namespace {

bool stripPrefixCaseInsensitive(std::string_view &str, std::string_view prefix)
{
    if (str.size() >= prefix.size() && Platform::equalsIgnoreCaseAscii(str.substr(0, prefix.size()), prefix)) {
        str.remove_prefix(prefix.size());
        return true;
    }
    return false;
}

std::string_view stripSchemeAndWww(std::string_view text)
{
    std::string_view s = Platform::trim(text);
    if (!stripPrefixCaseInsensitive(s, "https://"))
        stripPrefixCaseInsensitive(s, "http://");
    stripPrefixCaseInsensitive(s, "www.");
    return s;
}

std::optional<std::string_view> codeAfterHost(std::string_view text)
{
    std::string_view s = stripSchemeAndWww(text);

    static constexpr std::string_view hosts[] = {
        "discord.gg/",
        "discord.com/invite/",
        "discordapp.com/invite/",
        "ptb.discord.com/invite/",
        "canary.discord.com/invite/",
    };

    for (std::string_view host : hosts) {
        if (stripPrefixCaseInsensitive(s, host)) {
            const size_t end = s.find_first_of("/?#");
            if (end != std::string_view::npos)
                s = s.substr(0, end);
            return s;
        }
    }
    return std::nullopt;
}

bool validCode(std::string_view code)
{
    if (code.empty())
        return false;

    for (char c : code) {
        const bool isAsciiLetter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool isDigit = (c >= '0' && c <= '9');
        if (!isAsciiLetter && !isDigit && c != '-')
            return false;
    }
    return true;
}

std::string stringOrEmpty(const nlohmann::json &obj, const char *key)
{
    if (obj.is_object()) {
        const auto it = obj.find(key);
        if (it != obj.end() && it->is_string())
            return it->get<std::string>();
    }
    return {};
}

} // namespace

std::optional<std::string> parseInviteCode(std::string_view text)
{
    const auto hostCode = codeAfterHost(text);
    const std::string_view code = hostCode ? *hostCode : stripSchemeAndWww(text);
    if (!validCode(code))
        return std::nullopt;
    return std::string(code);
}

std::optional<std::string> parseInviteLink(std::string_view text)
{
    const auto code = codeAfterHost(text);
    if (!code || !validCode(*code))
        return std::nullopt;
    return std::string(*code);
}

InvitePreview parseInvitePreview(const nlohmann::json &invite)
{
    InvitePreview preview;
    if (!invite.is_object())
        return preview;

    preview.code = stringOrEmpty(invite, "code");

    const auto itGuild = invite.find("guild");
    if (itGuild != invite.end() && itGuild->is_object()) {
        preview.guildId = Core::JsonUtils::detail::snowflake(stringOrEmpty(*itGuild, "id"));
        preview.guildName = stringOrEmpty(*itGuild, "name");
        preview.guildIcon = stringOrEmpty(*itGuild, "icon");
    }

    const auto itChannel = invite.find("channel");
    if (itChannel != invite.end() && itChannel->is_object()) {
        preview.channelId = Core::JsonUtils::detail::snowflake(stringOrEmpty(*itChannel, "id"));
        preview.channelName = stringOrEmpty(*itChannel, "name");
    }

    const auto itInviter = invite.find("inviter");
    if (itInviter != invite.end() && itInviter->is_object()) {
        std::string globalName = stringOrEmpty(*itInviter, "global_name");
        preview.inviterName = !globalName.empty() ? std::move(globalName) : stringOrEmpty(*itInviter, "username");
    }

    const auto itMembers = invite.find("approximate_member_count");
    if (itMembers != invite.end() && itMembers->is_number_integer())
        preview.memberCount = itMembers->get<int>();

    const auto itOnline = invite.find("approximate_presence_count");
    if (itOnline != invite.end() && itOnline->is_number_integer())
        preview.onlineCount = itOnline->get<int>();

    const std::string expiresAt = stringOrEmpty(invite, "expires_at");
    if (!expiresAt.empty())
        preview.expiresAt = Platform::DateTime::parse(expiresAt);

    return preview;
}

} // namespace Styx::Discord
