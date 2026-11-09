#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Native.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace Styx::Discord {

// The invite code in a raw code ("abc123") or an invite link: discord.gg/abc123,
// discord.com/invite/abc123, discordapp.com/invite/abc123 and the ptb./canary. hosts, with or
// without https:// and www., ignoring any query or fragment. Nothing for anything else.
[[nodiscard]] std::optional<std::string> parseInviteCode(std::string_view text);

// What GET /invites/{code}?with_counts=true&with_expiration=true says about an invite.
struct InvitePreview
{
    std::string code;
    Core::Snowflake guildId;
    std::string guildName;
    // CDN hash; empty when the server has no icon.
    std::string guildIcon;
    Core::Snowflake channelId;
    std::string channelName;
    // Display name (global name, else username); empty when there's no inviter.
    std::string inviterName;
    int memberCount = 0;
    int onlineCount = 0;
    // Invalid when the invite doesn't expire.
    Platform::DateTime expiresAt;
};

[[nodiscard]] InvitePreview parseInvitePreview(const nlohmann::json &invite);

} // namespace Styx::Discord
