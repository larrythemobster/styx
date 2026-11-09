#include "Discord/GuildCreate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace Styx::Discord;

TEST_CASE("guildCreateBody serializes blank server with null icon when empty", "[guild-create]")
{
    const std::string expected = R"({"name":"Styx's server","icon":null,"channels":[],"system_channel_id":null,"guild_template_code":"2TffvPucqHkN"})";
    REQUIRE(guildCreateBody("Styx's server", "").toBytes() == expected);
}

TEST_CASE("guildCreateBody includes icon data URI when provided", "[guild-create]")
{
    const std::string expected = R"({"name":"Styx's server","icon":"data:image/png;base64,AAAA","channels":[],"system_channel_id":null,"guild_template_code":"2TffvPucqHkN"})";
    REQUIRE(guildCreateBody("Styx's server", "data:image/png;base64,AAAA").toBytes() == expected);
}

TEST_CASE("guildCreateBody escapes quotes and preserves UTF-8 in guild name", "[guild-create]")
{
    const std::string name = std::string("Styx's \"caf\xC3\xA9\" server");
    // Keep the escaped raw string outside the assertion macro for Visual Studio.
    const std::string expected = std::string(R"({"name":"Styx's \"caf)") +
                                 std::string("\xC3\xA9") +
                                 std::string(R"(\" server","icon":null,"channels":[],"system_channel_id":null,"guild_template_code":"2TffvPucqHkN"})");
    REQUIRE(guildCreateBody(name, "").toBytes() == expected);
}
