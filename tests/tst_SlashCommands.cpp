#include "UI/Chat/SlashCommands.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace Styx;
using namespace Styx::UI;
using namespace Styx::Discord;
using Styx::Core::Snowflake;

TEST_CASE("Flattening extracts subcommand leaves and plain commands excluding non-chat commands", "[commands]")
{
    const ApplicationCommandOption subBan{
        .type = ApplicationCommandOptionType::SUB_COMMAND,
        .name = "ban",
        .description = "Ban user",
        .options = {{.type = ApplicationCommandOptionType::USER, .name = "target"}},
    };
    const ApplicationCommandOption subKick{
        .type = ApplicationCommandOptionType::SUB_COMMAND,
        .name = "kick",
        .description = "Kick user",
        .options = {{.type = ApplicationCommandOptionType::USER, .name = "target"}},
    };
    const ApplicationCommandOption groupUser{
        .type = ApplicationCommandOptionType::SUB_COMMAND_GROUP,
        .name = "user",
        .options = {subBan, subKick},
    };
    const ApplicationCommandOption subPurge{
        .type = ApplicationCommandOptionType::SUB_COMMAND,
        .name = "purge",
        .description = "Purge messages",
        .options = {{.type = ApplicationCommandOptionType::INTEGER, .name = "count"}},
    };
    const ApplicationCommand mod{
        .id = Snowflake(1),
        .type = ApplicationCommandType::CHAT_INPUT,
        .name = "mod",
        .description = "Mod tools",
        .options = {groupUser, subPurge},
    };
    const ApplicationCommand ping{
        .id = Snowflake(2),
        .type = ApplicationCommandType::CHAT_INPUT,
        .name = "ping",
        .description = "Ping",
        .options = {{.type = ApplicationCommandOptionType::STRING, .name = "payload"}},
    };
    const ApplicationCommand userCmd{
        .id = Snowflake(3),
        .type = ApplicationCommandType::USER,
        .name = "inspect",
    };

    const ApplicationCommandIndex index{
        .commands = {mod, ping, userCmd},
    };

    const auto cmds = availableSlashCommands(&index, nullptr, CommandContext::Guild);
    REQUIRE(cmds.size() == 4);

    CHECK(cmds[0].displayName == "mod purge");
    CHECK(cmds[0].subcommandPath == std::vector<std::string>{"purge"});
    CHECK(cmds[0].description == "Purge messages");
    REQUIRE(cmds[0].options.size() == 1);
    CHECK(cmds[0].options[0].name == "count");

    CHECK(cmds[1].displayName == "mod user ban");
    CHECK(cmds[1].subcommandPath == std::vector<std::string>{"user", "ban"});
    CHECK(cmds[1].description == "Ban user");
    REQUIRE(cmds[1].options.size() == 1);
    CHECK(cmds[1].options[0].name == "target");

    CHECK(cmds[2].displayName == "mod user kick");
    CHECK(cmds[2].subcommandPath == std::vector<std::string>{"user", "kick"});
    CHECK(cmds[2].description == "Kick user");
    REQUIRE(cmds[2].options.size() == 1);
    CHECK(cmds[2].options[0].name == "target");

    CHECK(cmds[3].displayName == "ping");
    CHECK(cmds[3].subcommandPath.empty());
    CHECK(cmds[3].description == "Ping");
    REQUIRE(cmds[3].options.size() == 1);
    CHECK(cmds[3].options[0].type == ApplicationCommandOptionType::STRING);
    CHECK(cmds[3].options[0].name == "payload");
}

TEST_CASE("Context filtering restricts commands according to allowed interaction contexts", "[commands]")
{
    const ApplicationCommand guildOnly{
        .id = Snowflake(1),
        .name = "guild_cmd",
        .contexts = {0},
    };
    const ApplicationCommand everywhere{
        .id = Snowflake(2),
        .name = "universal_cmd",
        .contexts = {},
    };
    const ApplicationCommand dmOrPrivate{
        .id = Snowflake(3),
        .name = "private_cmd",
        .contexts = {1, 2},
    };

    const ApplicationCommandIndex index{
        .commands = {guildOnly, everywhere, dmOrPrivate},
    };

    const auto guildCmds = availableSlashCommands(&index, nullptr, CommandContext::Guild);
    REQUIRE(guildCmds.size() == 2);
    CHECK(guildCmds[0].name == "guild_cmd");
    CHECK(guildCmds[1].name == "universal_cmd");

    const auto botDmCmds = availableSlashCommands(&index, nullptr, CommandContext::BotDm);
    REQUIRE(botDmCmds.size() == 2);
    CHECK(botDmCmds[0].name == "private_cmd");
    CHECK(botDmCmds[1].name == "universal_cmd");

    const auto privateCmds = availableSlashCommands(&index, nullptr, CommandContext::PrivateChannel);
    REQUIRE(privateCmds.size() == 2);
    CHECK(privateCmds[0].name == "private_cmd");
    CHECK(privateCmds[1].name == "universal_cmd");
}

TEST_CASE("Command deduplication and application name resolution across scope and user indexes", "[commands]")
{
    const ApplicationCommandIndex scopeIndex{
        .applications = {{.id = Snowflake(10), .name = "ScopeApp"}},
        .commands = {
            {.id = Snowflake(1), .applicationId = Snowflake(10), .name = "common"},
        },
    };
    const ApplicationCommandIndex userIndex{
        .applications = {
            {.id = Snowflake(10), .name = "ScopeApp"},
            {.id = Snowflake(20), .name = "UserApp"},
        },
        .commands = {
            {.id = Snowflake(1), .applicationId = Snowflake(10), .name = "common"},
            {.id = Snowflake(2), .applicationId = Snowflake(20), .name = "user_only"},
        },
    };

    const auto merged = availableSlashCommands(&scopeIndex, &userIndex, CommandContext::Guild);
    REQUIRE(merged.size() == 2);
    CHECK(merged[0].displayName == "common");
    CHECK(merged[0].applicationName == "ScopeApp");
    CHECK(merged[1].displayName == "user_only");
    CHECK(merged[1].applicationName == "UserApp");

    const auto empty = availableSlashCommands(nullptr, nullptr, CommandContext::Guild);
    REQUIRE(empty.empty());
}

TEST_CASE("Slash query parsing and command matching with prefix and inner substring filtering", "[commands]")
{
    const std::vector<SlashCommandEntry> entries{
        {.displayName = "ban"},
        {.displayName = "mod user ban"},
        {.displayName = "unban"},
    };

    const auto matched = matchSlashCommands(entries, "ban", 10);
    REQUIRE(matched == std::vector<std::size_t>{0, 1, 2});

    const auto matchedCase = matchSlashCommands(entries, "BAN", 10);
    REQUIRE(matchedCase == std::vector<std::size_t>{0, 1, 2});

    const auto limited = matchSlashCommands(entries, "ban", 1);
    REQUIRE(limited == std::vector<std::size_t>{0});

    CHECK(slashQuery("/mo") == "mo");
    REQUIRE_FALSE(slashQuery("hi").has_value());
    REQUIRE_FALSE(slashQuery("/a\nb").has_value());
    CHECK(slashQuery("/") == "");
}
