#include "Discord/Events.hpp"
#include "Discord/Enums.hpp"
#include "Core/Snowflake.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string_view>

using namespace Styx::Discord;
using Styx::Core::Snowflake;

TEST_CASE("Gateway maps channel recipient event names to GatewayEvent enums", "[group-dm-events]")
{
    const struct {
        std::string_view name;
        GatewayEvent expected;
    } cases[] = {
        { "CHANNEL_RECIPIENT_ADD", GatewayEvent::CHANNEL_RECIPIENT_ADD },
        { "CHANNEL_RECIPIENT_REMOVE", GatewayEvent::CHANNEL_RECIPIENT_REMOVE },
    };

    for (const auto &c : cases)
        REQUIRE(parseGatewayEvent(c.name) == c.expected);
}

TEST_CASE("ChannelRecipient parses recipient add payload with nick and user identity", "[group-dm-events]")
{
    const auto fixture = nlohmann::json::parse(R"({
        "channel_id": "111222333444555666",
        "user": {
            "id": "222333444555666777",
            "username": "added_member",
            "global_name": "Added Member",
            "avatar": "abcdef0123456789abcdef0123456789"
        },
        "nick": "MemeMaster"
    })");

    const auto event = ChannelRecipient::fromJson(fixture);
    REQUIRE(event.channelId == Snowflake(111222333444555666ULL));
    REQUIRE(event.user.get().id == Snowflake(222333444555666777ULL));
    REQUIRE(event.user.get().username.get() == "added_member");
    REQUIRE(event.user.get().globalName.get() == "Added Member");
    REQUIRE(event.user.get().avatar.get() == "abcdef0123456789abcdef0123456789");
    REQUIRE(event.nick.hasValue());
    REQUIRE(event.nick.get() == "MemeMaster");
}

TEST_CASE("ChannelRecipient parses recipient remove payload with missing nick and id-only user", "[group-dm-events]")
{
    const auto fixture = nlohmann::json::parse(R"({
        "channel_id": "111222333444555666",
        "user": {
            "id": "333444555666777888"
        }
    })");

    const auto event = ChannelRecipient::fromJson(fixture);
    REQUIRE(event.channelId == Snowflake(111222333444555666ULL));
    REQUIRE(event.user.get().id == Snowflake(333444555666777888ULL));
    REQUIRE(event.nick.isUndefined());
}
