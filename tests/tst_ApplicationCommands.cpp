#include "Discord/ApplicationCommands.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <vector>

using namespace Styx;
using namespace Styx::Discord;
using Styx::Core::Snowflake;

TEST_CASE("parseApplicationCommandIndex parses full index with applications commands and nested options", "[commands]")
{
    const auto doc = nlohmann::json::parse(R"({
        "version": "1234567890",
        "applications": [
            {
                "id": "1001",
                "name": "Test Application",
                "description": "An application description",
                "icon": "app_icon_hash",
                "bot_id": "2001"
            }
        ],
        "application_commands": [
            {
                "id": "3001",
                "application_id": "1001",
                "name": "cmd",
                "description": "Command description",
                "type": 1,
                "version": "4001",
                "nsfw": false,
                "contexts": [0, 1],
                "options": [
                    {
                        "type": 1,
                        "name": "subcmd",
                        "description": "Subcommand description",
                        "options": [
                            {
                                "type": 3,
                                "name": "str_opt",
                                "description": "String option description",
                                "required": true,
                                "choices": [
                                    {
                                        "name": "choice_str",
                                        "value": "string_val"
                                    },
                                    {
                                        "name": "choice_int",
                                        "value": 5
                                    }
                                ]
                            },
                            {
                                "type": 4,
                                "name": "int_opt",
                                "description": "Integer option description",
                                "autocomplete": true,
                                "min_value": 1,
                                "max_value": 10.5
                            },
                            {
                                "type": 7,
                                "name": "chan_opt",
                                "description": "Channel option description",
                                "channel_types": [0, 5]
                            }
                        ]
                    }
                ]
            }
        ]
    })");

    const ApplicationCommandIndex index = parseApplicationCommandIndex(doc);
    REQUIRE(index.version == Snowflake(1234567890ULL));

    REQUIRE(index.applications.size() == 1);
    const auto &app = index.applications[0];
    REQUIRE(app.id == Snowflake(1001));
    REQUIRE(app.name == "Test Application");
    REQUIRE(app.description == "An application description");
    REQUIRE(app.icon == "app_icon_hash");
    REQUIRE(app.botId == Snowflake(2001));

    REQUIRE(index.commands.size() == 1);
    const auto &cmd = index.commands[0];
    REQUIRE(cmd.id == Snowflake(3001));
    REQUIRE(cmd.applicationId == Snowflake(1001));
    REQUIRE(cmd.guildId == Snowflake::Invalid);
    REQUIRE_FALSE(cmd.guildId.isValid());
    REQUIRE(cmd.name == "cmd");
    REQUIRE(cmd.description == "Command description");
    REQUIRE(cmd.type == ApplicationCommandType::CHAT_INPUT);
    REQUIRE(cmd.version == Snowflake(4001));
    REQUIRE_FALSE(cmd.nsfw);
    const std::vector<int> expectedContexts = { 0, 1 };
    REQUIRE(cmd.contexts == expectedContexts);

    REQUIRE(cmd.options.size() == 1);
    const auto &sub = cmd.options[0];
    REQUIRE(sub.type == ApplicationCommandOptionType::SUB_COMMAND);
    REQUIRE(sub.name == "subcmd");
    REQUIRE(sub.description == "Subcommand description");

    REQUIRE(sub.options.size() == 3);
    const auto &opt0 = sub.options[0];
    REQUIRE(opt0.type == ApplicationCommandOptionType::STRING);
    REQUIRE(opt0.name == "str_opt");
    REQUIRE(opt0.description == "String option description");
    REQUIRE(opt0.required);
    REQUIRE_FALSE(opt0.autocomplete);
    REQUIRE(opt0.choices.size() == 2);
    REQUIRE(opt0.choices[0].name == "choice_str");
    REQUIRE(opt0.choices[0].value == nlohmann::json("string_val"));
    REQUIRE(opt0.choices[1].name == "choice_int");
    REQUIRE(opt0.choices[1].value == nlohmann::json(5));

    const auto &opt1 = sub.options[1];
    REQUIRE(opt1.type == ApplicationCommandOptionType::INTEGER);
    REQUIRE(opt1.name == "int_opt");
    REQUIRE(opt1.description == "Integer option description");
    REQUIRE_FALSE(opt1.required);
    REQUIRE(opt1.autocomplete);
    REQUIRE(opt1.minValue.has_value());
    REQUIRE(*opt1.minValue == 1.0);
    REQUIRE(opt1.maxValue.has_value());
    REQUIRE(*opt1.maxValue == 10.5);

    const auto &opt2 = sub.options[2];
    REQUIRE(opt2.type == ApplicationCommandOptionType::CHANNEL);
    REQUIRE(opt2.name == "chan_opt");
    REQUIRE(opt2.description == "Channel option description");
    const std::vector<int> expectedChannelTypes = { 0, 5 };
    REQUIRE(opt2.channelTypes == expectedChannelTypes);
}

TEST_CASE("parseApplicationCommandIndex skips malformed entries and returns empty lists for non-objects", "[commands]")
{
    const auto doc = nlohmann::json::parse(R"({
        "version": "9999",
        "applications": [
            {
                "id": "100",
                "name": "Valid App 1"
            },
            {
                "id": "101"
            },
            {
                "id": "102",
                "name": "Valid App 2"
            }
        ],
        "application_commands": [
            {
                "id": "200",
                "application_id": "100",
                "name": "valid_cmd_1"
            },
            {
                "id": "201",
                "name": "missing_app_id"
            },
            {
                "id": "202",
                "application_id": "100",
                "name": "valid_cmd_2",
                "options": [
                    {
                        "type": 3,
                        "name": "opt",
                        "choices": [
                            {
                                "name": "choice_good_1",
                                "value": "val1"
                            },
                            {
                                "name": "choice_bad_noval"
                            },
                            {
                                "name": "choice_good_2",
                                "value": 42
                            }
                        ]
                    }
                ]
            }
        ]
    })");

    const ApplicationCommandIndex index = parseApplicationCommandIndex(doc);
    REQUIRE(index.applications.size() == 2);
    REQUIRE(index.applications[0].id == Snowflake(100));
    REQUIRE(index.applications[0].name == "Valid App 1");
    REQUIRE(index.applications[1].id == Snowflake(102));
    REQUIRE(index.applications[1].name == "Valid App 2");

    REQUIRE(index.commands.size() == 2);
    REQUIRE(index.commands[0].id == Snowflake(200));
    REQUIRE(index.commands[0].name == "valid_cmd_1");
    REQUIRE(index.commands[1].id == Snowflake(202));
    REQUIRE(index.commands[1].name == "valid_cmd_2");

    REQUIRE(index.commands[1].options.size() == 1);
    const auto &choices = index.commands[1].options[0].choices;
    REQUIRE(choices.size() == 2);
    REQUIRE(choices[0].name == "choice_good_1");
    REQUIRE(choices[0].value == nlohmann::json("val1"));
    REQUIRE(choices[1].name == "choice_good_2");
    REQUIRE(choices[1].value == nlohmann::json(42));

    const auto nonObject = parseApplicationCommandIndex(nlohmann::json::array());
    REQUIRE(nonObject.applications.empty());
    REQUIRE(nonObject.commands.empty());

    const auto missingArrays = parseApplicationCommandIndex(nlohmann::json::object());
    REQUIRE(missingArrays.applications.empty());
    REQUIRE(missingArrays.commands.empty());
}

TEST_CASE("parseApplicationCommandIndex handles null icons absent command types and user commands", "[commands]")
{
    const auto doc = nlohmann::json::parse(R"({
        "applications": [
            {
                "id": "500",
                "name": "App With Null Icon",
                "icon": null
            }
        ],
        "application_commands": [
            {
                "id": "600",
                "application_id": "500",
                "name": "type_absent_cmd"
            },
            {
                "id": "601",
                "application_id": "500",
                "name": "user_cmd",
                "type": 2
            }
        ]
    })");

    const ApplicationCommandIndex index = parseApplicationCommandIndex(doc);
    REQUIRE(index.applications.size() == 1);
    REQUIRE(index.applications[0].icon.empty());

    REQUIRE(index.commands.size() == 2);
    REQUIRE(index.commands[0].type == ApplicationCommandType::CHAT_INPUT);
    REQUIRE(index.commands[1].type == ApplicationCommandType::USER);
}
