#include "Discord/InviteLink.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>

using namespace Styx;
using namespace Styx::Discord;
using Styx::Core::Snowflake;

TEST_CASE("parseInviteCode extracts invite codes from various link formats and raw codes", "[invite]")
{
    REQUIRE(parseInviteCode("abc123") == "abc123");
    REQUIRE(parseInviteCode("discord.gg/abc123") == "abc123");
    REQUIRE(parseInviteCode("https://discord.gg/AbC-12?event=1") == "AbC-12");
    REQUIRE(parseInviteCode("HTTPS://WWW.Discord.com/invite/xyz/") == "xyz");
    REQUIRE(parseInviteCode("discordapp.com/invite/q1") == "q1");
    REQUIRE(parseInviteCode("https://ptb.discord.com/invite/q2#x") == "q2");
    REQUIRE(parseInviteCode("canary.discord.com/invite/q3") == "q3");
    REQUIRE(parseInviteCode("  discord.gg/pad  ") == "pad");

    REQUIRE_FALSE(parseInviteCode("").has_value());
    REQUIRE_FALSE(parseInviteCode("discord.gg/").has_value());
    REQUIRE_FALSE(parseInviteCode("https://example.com/x").has_value());
    REQUIRE_FALSE(parseInviteCode("has space").has_value());
    REQUIRE_FALSE(parseInviteCode("discord.com/channels/1/2").has_value());
}

TEST_CASE("parseInviteLink extracts codes only from invite links", "[invite]")
{
    REQUIRE(parseInviteLink("https://discord.gg/abc") == "abc");
    REQUIRE(parseInviteLink("discord.com/invite/xyz?x=1") == "xyz");

    REQUIRE_FALSE(parseInviteLink("abc123").has_value());
    REQUIRE_FALSE(parseInviteLink("https://example.com").has_value());
    REQUIRE_FALSE(parseInviteLink("https://example.com/invite/abc").has_value());
    REQUIRE_FALSE(parseInviteLink("https://discord.com/channels/1/2").has_value());
    REQUIRE_FALSE(parseInviteLink("https://foo").has_value());
}

TEST_CASE("parseInvitePreview parses all fields from a full invite payload", "[invite]")
{
    const auto doc = nlohmann::json::parse(R"({
        "code": "full-invite",
        "guild": {
            "id": "1234567890123456789",
            "name": "Styx Dev",
            "icon": "abcdef0123456789"
        },
        "channel": {
            "id": "987654321098765432",
            "name": "general"
        },
        "inviter": {
            "username": "styx_user",
            "global_name": "Styx Developer"
        },
        "approximate_member_count": 4200,
        "approximate_presence_count": 1337,
        "expires_at": "2026-11-09T18:30:00Z"
    })");

    const InvitePreview preview = parseInvitePreview(doc);
    REQUIRE(preview.code == "full-invite");
    REQUIRE(preview.guildId == Snowflake(1234567890123456789ULL));
    REQUIRE(preview.guildName == "Styx Dev");
    REQUIRE(preview.guildIcon == "abcdef0123456789");
    REQUIRE(preview.channelId == Snowflake(987654321098765432ULL));
    REQUIRE(preview.channelName == "general");
    REQUIRE(preview.inviterName == "Styx Developer");
    REQUIRE(preview.memberCount == 4200);
    REQUIRE(preview.onlineCount == 1337);
    REQUIRE(preview.expiresAt.valid());
    REQUIRE(preview.expiresAt == Platform::DateTime::parse("2026-11-09T18:30:00Z"));
}

TEST_CASE("parseInvitePreview falls back to username when global_name is null", "[invite]")
{
    const auto doc = nlohmann::json::parse(R"({
        "code": "fallback-invite",
        "inviter": {
            "username": "plain_user",
            "global_name": null
        }
    })");

    const InvitePreview preview = parseInvitePreview(doc);
    REQUIRE(preview.inviterName == "plain_user");
}

TEST_CASE("parseInvitePreview populates defaults when optional fields are null or missing", "[invite]")
{
    const auto doc = nlohmann::json::parse(R"({
        "code": "minimal-invite",
        "guild": {
            "id": "111222333444555666",
            "name": "Minimal Guild",
            "icon": null
        },
        "channel": {
            "id": "777888999000111222",
            "name": null
        }
    })");

    const InvitePreview preview = parseInvitePreview(doc);
    REQUIRE(preview.code == "minimal-invite");
    REQUIRE(preview.guildId == Snowflake(111222333444555666ULL));
    REQUIRE(preview.guildName == "Minimal Guild");
    REQUIRE(preview.guildIcon.empty());
    REQUIRE(preview.channelId == Snowflake(777888999000111222ULL));
    REQUIRE(preview.channelName.empty());
    REQUIRE(preview.inviterName.empty());
    REQUIRE(preview.memberCount == 0);
    REQUIRE(preview.onlineCount == 0);
    REQUIRE_FALSE(preview.expiresAt.valid());
}
