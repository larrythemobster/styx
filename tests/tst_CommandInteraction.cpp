#include "Discord/CommandInteraction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace Styx::Discord;

namespace {

ApplicationCommandRequest createBaseRequest()
{
    ApplicationCommandRequest req;
    req.applicationId = Styx::Core::Snowflake(1001);
    req.guildId = Styx::Core::Snowflake(1002);
    req.channelId = Styx::Core::Snowflake(1003);
    req.commandId = Styx::Core::Snowflake(2001);
    req.commandVersion = Styx::Core::Snowflake(3001);
    req.commandName = "testcmd";
    req.nonce = "nonce_1";
    return req;
}

} // namespace

TEST_CASE("commandOptionValue parses types and respects bounds and choices", "[commands]")
{
    ApplicationCommandOption option;

    // STRING
    option.type = ApplicationCommandOptionType::STRING;
    option.minLength = 2;
    REQUIRE_FALSE(commandOptionValue(option, "a").success());

    option.minLength.reset();
    option.maxLength = 1;
    const auto stringResult = commandOptionValue(option, "é");
    REQUIRE(stringResult.success());
    REQUIRE(*stringResult.value == "é");

    const auto strOver = commandOptionValue(option, "ab");
    REQUIRE_FALSE(strOver.success());
    REQUIRE(strOver.error == "Must be at most 1 character");

    // INTEGER
    option.type = ApplicationCommandOptionType::INTEGER;
    option.maxLength.reset();
    const auto intResult = commandOptionValue(option, "12");
    REQUIRE(intResult.success());
    REQUIRE(intResult.value->is_number_integer());
    REQUIRE(*intResult.value == 12);

    REQUIRE(*commandOptionValue(option, " 12 ").value == 12);
    REQUIRE(*commandOptionValue(option, "+5").value == 5);
    REQUIRE_FALSE(commandOptionValue(option, "1.5").success());
    REQUIRE_FALSE(commandOptionValue(option, "1e3").success());

    option.minValue = 10.0;
    REQUIRE_FALSE(commandOptionValue(option, "9").success());
    option.minValue.reset();

    option.maxValue = 1000000.0;
    const auto intMax = commandOptionValue(option, "1000001");
    REQUIRE_FALSE(intMax.success());
    REQUIRE(intMax.error == "Must be at most 1000000");
    option.maxValue.reset();

    const auto intMaxSafe = commandOptionValue(option, "9007199254740992");
    REQUIRE(intMaxSafe.success());
    REQUIRE(*intMaxSafe.value == 9007199254740992LL);
    REQUIRE_FALSE(commandOptionValue(option, "9007199254740993").success());

    // NUMBER
    option.type = ApplicationCommandOptionType::NUMBER;
    const auto numResult = commandOptionValue(option, "2.5");
    REQUIRE(numResult.success());
    REQUIRE(numResult.value->is_number_float());
    REQUIRE(*numResult.value == 2.5);

    const auto numExp = commandOptionValue(option, "1e3");
    REQUIRE(numExp.success());
    REQUIRE(*numExp.value == 1000.0);
    REQUIRE_FALSE(commandOptionValue(option, "inf").success());

    // BOOLEAN
    option.type = ApplicationCommandOptionType::BOOLEAN;
    const auto boolResult = commandOptionValue(option, "TRUE");
    REQUIRE(boolResult.success());
    REQUIRE(*boolResult.value == true);

    // USER
    option.type = ApplicationCommandOptionType::USER;
    const auto userResult = commandOptionValue(option, "123");
    REQUIRE(userResult.success());
    REQUIRE(*userResult.value == "123");

    const auto userPad = commandOptionValue(option, "00123");
    REQUIRE(userPad.success());
    REQUIRE(*userPad.value == "123");

    REQUIRE_FALSE(commandOptionValue(option, "0").success());
    REQUIRE_FALSE(commandOptionValue(option, "abc").success());

    // Choices with name shadowing
    option.type = ApplicationCommandOptionType::INTEGER;
    option.choices = { { "1", 60 }, { "60", 3600 } };
    const auto choiceByName = commandOptionValue(option, "60");
    REQUIRE(choiceByName.success());
    REQUIRE(*choiceByName.value == 3600);

    const auto choiceByVal = commandOptionValue(option, "3600");
    REQUIRE(choiceByVal.success());
    REQUIRE(*choiceByVal.value == 3600);

    // ATTACHMENT
    option.choices.clear();
    option.type = ApplicationCommandOptionType::ATTACHMENT;
    REQUIRE_FALSE(commandOptionValue(option, "file.png").success());

    // Whitespace only
    option.type = ApplicationCommandOptionType::STRING;
    REQUIRE_FALSE(commandOptionValue(option, "  ").success());
}

TEST_CASE("applicationCommandBody serializes group and subcommand with declared order", "[commands]")
{
    auto req = createBaseRequest();
    req.commandName = "manage";
    req.subcommandPath = { "config", "set" };

    ApplicationCommandOption optA;
    optA.name = "key";
    optA.type = ApplicationCommandOptionType::STRING;

    ApplicationCommandOption optB;
    optB.name = "count";
    optB.type = ApplicationCommandOptionType::INTEGER;

    req.declaredOptions = { optA, optB };
    req.values = {
        { "count", ApplicationCommandOptionType::INTEGER, 42 },
        { "key", ApplicationCommandOptionType::STRING, "limit" }
    };

    const auto result = applicationCommandBody(req, "sess_123");
    REQUIRE(result.success());

    const auto expected = nlohmann::json::parse(R"({
        "type": 2,
        "application_id": "1001",
        "channel_id": "1003",
        "guild_id": "1002",
        "session_id": "sess_123",
        "nonce": "nonce_1",
        "data": {
            "id": "2001",
            "type": 1,
            "name": "manage",
            "version": "3001",
            "options": [
                {
                    "type": 2,
                    "name": "config",
                    "options": [
                        {
                            "type": 1,
                            "name": "set",
                            "options": [
                                {
                                    "type": 3,
                                    "name": "key",
                                    "value": "limit"
                                },
                                {
                                    "type": 4,
                                    "name": "count",
                                    "value": 42
                                }
                            ]
                        }
                    ]
                }
            ]
        }
    })");

    REQUIRE(result.value->dump() == expected.dump());
}

TEST_CASE("applicationCommandBody serializes single subcommand path", "[commands]")
{
    auto req = createBaseRequest();
    req.commandName = "tag";
    req.subcommandPath = { "show" };

    ApplicationCommandOption opt;
    opt.name = "name";
    opt.type = ApplicationCommandOptionType::STRING;

    req.declaredOptions = { opt };
    req.values = {
        { "name", ApplicationCommandOptionType::STRING, "styx" }
    };

    const auto result = applicationCommandBody(req, "sess_123");
    REQUIRE(result.success());

    const auto expected = nlohmann::json::parse(R"({
        "type": 2,
        "application_id": "1001",
        "channel_id": "1003",
        "guild_id": "1002",
        "session_id": "sess_123",
        "nonce": "nonce_1",
        "data": {
            "id": "2001",
            "type": 1,
            "name": "tag",
            "version": "3001",
            "options": [
                {
                    "type": 1,
                    "name": "show",
                    "options": [
                        {
                            "type": 3,
                            "name": "name",
                            "value": "styx"
                        }
                    ]
                }
            ]
        }
    })");

    REQUIRE(result.value->dump() == expected.dump());
}

TEST_CASE("applicationCommandBody serializes plain command without path or guild", "[commands]")
{
    auto req = createBaseRequest();
    req.guildId = Styx::Core::Snowflake::Invalid;
    req.commandName = "ping";
    req.subcommandPath.clear();

    ApplicationCommandOption opt;
    opt.name = "verbose";
    opt.type = ApplicationCommandOptionType::BOOLEAN;

    req.declaredOptions = { opt };
    req.values = {
        { "verbose", ApplicationCommandOptionType::BOOLEAN, true }
    };

    const auto result = applicationCommandBody(req, "sess_dm");
    REQUIRE(result.success());
    REQUIRE_FALSE((*result.value).contains("guild_id"));

    const auto expected = nlohmann::json::parse(R"({
        "type": 2,
        "application_id": "1001",
        "channel_id": "1003",
        "session_id": "sess_dm",
        "nonce": "nonce_1",
        "data": {
            "id": "2001",
            "type": 1,
            "name": "ping",
            "version": "3001",
            "options": [
                {
                    "type": 5,
                    "name": "verbose",
                    "value": true
                }
            ]
        }
    })");

    REQUIRE(result.value->dump() == expected.dump());
}

TEST_CASE("applicationCommandBody rejects invalid requests", "[commands]")
{
    auto req = createBaseRequest();

    ApplicationCommandOption requiredOpt;
    requiredOpt.name = "query";
    requiredOpt.type = ApplicationCommandOptionType::STRING;
    requiredOpt.required = true;
    req.declaredOptions = { requiredOpt };

    // Missing required
    const auto missingReq = applicationCommandBody(req, "sess_1");
    REQUIRE_FALSE(missingReq.success());
    REQUIRE(missingReq.error == "query is required");

    // Unknown option (supply required so it fails specifically for unknown option)
    req.values = {
        { "query", ApplicationCommandOptionType::STRING, "val" },
        { "unknown", ApplicationCommandOptionType::STRING, "val" }
    };
    const auto unknownOpt = applicationCommandBody(req, "sess_1");
    REQUIRE_FALSE(unknownOpt.success());
    REQUIRE(unknownOpt.error == "Unknown option unknown");

    // Duplicate option
    req.values = {
        { "query", ApplicationCommandOptionType::STRING, "val1" },
        { "query", ApplicationCommandOptionType::STRING, "val2" }
    };
    const auto duplicateOpt = applicationCommandBody(req, "sess_1");
    REQUIRE_FALSE(duplicateOpt.success());
    REQUIRE(duplicateOpt.error == "Option query given twice");

    // Subcommand path of 3
    req.values = {
        { "query", ApplicationCommandOptionType::STRING, "val1" }
    };
    req.subcommandPath = { "one", "two", "three" };
    const auto deepPath = applicationCommandBody(req, "sess_1");
    REQUIRE_FALSE(deepPath.success());
    REQUIRE(deepPath.error == "Subcommand path is too deep");

    // Missing command version
    req.subcommandPath.clear();
    req.commandVersion = Styx::Core::Snowflake::Invalid;
    const auto missingVer = applicationCommandBody(req, "sess_1");
    REQUIRE_FALSE(missingVer.success());
    REQUIRE(missingVer.error == "Command version is required");

    // Empty session
    req.commandVersion = Styx::Core::Snowflake(3001);
    const auto emptySess = applicationCommandBody(req, "");
    REQUIRE_FALSE(emptySess.success());
    REQUIRE(emptySess.error == "Session ID is required");
}
