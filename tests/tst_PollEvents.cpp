#include "Discord/Events.hpp"
#include "Discord/Enums.hpp"
#include "Discord/Inbound.hpp"
#include "Core/Snowflake.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

using namespace Styx::Discord;
using Styx::Core::Snowflake;

TEST_CASE("Gateway maps poll vote events and parses shared payload", "[poll-events]")
{
    const auto payload = nlohmann::json::parse(R"({
        "user_id": "112233445566778899",
        "channel_id": "223344556677889900",
        "message_id": "334455667788990011",
        "guild_id": "445566778899001122",
        "answer_id": 42
    })");

    const struct {
        std::string_view name;
        GatewayEvent event;
    } cases[] = {
        { "MESSAGE_POLL_VOTE_ADD", GatewayEvent::MESSAGE_POLL_VOTE_ADD },
        { "MESSAGE_POLL_VOTE_REMOVE", GatewayEvent::MESSAGE_POLL_VOTE_REMOVE },
    };

    for (const auto &c : cases) {
        Inbound inbound;
        inbound.t = std::string(c.name);
        inbound.data = payload;
        REQUIRE(parseGatewayEvent(inbound.t.value()) == c.event);

        const auto vote = inbound.getData<MessagePollVote>();
        REQUIRE(vote.userId == Snowflake(112233445566778899ULL));
        REQUIRE(vote.channelId == Snowflake(223344556677889900ULL));
        REQUIRE(vote.messageId == Snowflake(334455667788990011ULL));
        REQUIRE(vote.guildId.hasValue());
        REQUIRE(vote.guildId.get() == Snowflake(445566778899001122ULL));
        REQUIRE(vote.answerId == 42);
    }
}

TEST_CASE("MessagePollVote leaves guildId undefined for DM payload without guild_id", "[poll-events]")
{
    const auto dmPayload = nlohmann::json::parse(R"({
        "user_id": "112233445566778899",
        "channel_id": "223344556677889900",
        "message_id": "334455667788990011",
        "answer_id": 42
    })");

    const auto vote = MessagePollVote::fromJson(dmPayload);
    REQUIRE(vote.userId == Snowflake(112233445566778899ULL));
    REQUIRE(vote.channelId == Snowflake(223344556677889900ULL));
    REQUIRE(vote.messageId == Snowflake(334455667788990011ULL));
    REQUIRE(vote.guildId.isUndefined());
    REQUIRE(vote.answerId == 42);
}
