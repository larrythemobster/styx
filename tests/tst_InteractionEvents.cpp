#include "Discord/Events.hpp"
#include "Discord/Enums.hpp"
#include "Core/Snowflake.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>

using namespace Styx::Discord;
using Styx::Core::Snowflake;

TEST_CASE("InteractionCreate parses id and opaque nonce from gateway dispatch", "[interaction-events]")
{
    const auto fixture = nlohmann::json::parse(R"({
        "t": "INTERACTION_CREATE",
        "d": {
            "id": "112233445566778899",
            "nonce": "opaque_nonce_create_1"
        }
    })");

    REQUIRE(parseGatewayEvent(fixture["t"].get<std::string>()) == GatewayEvent::INTERACTION_CREATE);

    const auto event = InteractionCreate::fromJson(fixture["d"]);
    REQUIRE(event.id == Snowflake(112233445566778899ULL));
    REQUIRE(event.nonce.hasValue());
    REQUIRE(event.nonce.get() == "opaque_nonce_create_1");
}

TEST_CASE("InteractionCreate leaves missing optional nonce undefined", "[interaction-events]")
{
    const auto fixture = nlohmann::json::parse(R"({
        "t": "INTERACTION_CREATE",
        "d": {
            "id": "112233445566778899"
        }
    })");

    const auto event = InteractionCreate::fromJson(fixture["d"]);
    REQUIRE(event.id == Snowflake(112233445566778899ULL));
    REQUIRE(event.nonce.isUndefined());
}

TEST_CASE("InteractionSuccess parses id and opaque nonce", "[interaction-events]")
{
    const auto fixture = nlohmann::json::parse(R"({
        "t": "INTERACTION_SUCCESS",
        "d": {
            "id": "223344556677889900",
            "nonce": "opaque_nonce_success_1"
        }
    })");

    REQUIRE(parseGatewayEvent(fixture["t"].get<std::string>()) == GatewayEvent::INTERACTION_SUCCESS);

    const auto event = InteractionSuccess::fromJson(fixture["d"]);
    REQUIRE(event.id == Snowflake(223344556677889900ULL));
    REQUIRE(event.nonce.hasValue());
    REQUIRE_FALSE(event.nonce.isNull());
    REQUIRE(event.nonce.get() == "opaque_nonce_success_1");
}

TEST_CASE("InteractionSuccess preserves explicit null nonce", "[interaction-events]")
{
    const auto fixture = nlohmann::json::parse(R"({
        "t": "INTERACTION_SUCCESS",
        "d": {
            "id": "223344556677889900",
            "nonce": null
        }
    })");

    const auto event = InteractionSuccess::fromJson(fixture["d"]);
    REQUIRE(event.id == Snowflake(223344556677889900ULL));
    REQUIRE(event.nonce.isNull());
}

TEST_CASE("InteractionFailure parses id, nonce, and timeout reason code", "[interaction-events]")
{
    const auto fixture = nlohmann::json::parse(R"({
        "t": "INTERACTION_FAILURE",
        "d": {
            "id": "334455667788990011",
            "nonce": "opaque_nonce_failure_1",
            "reason_code": 2
        }
    })");

    REQUIRE(parseGatewayEvent(fixture["t"].get<std::string>()) == GatewayEvent::INTERACTION_FAILURE);

    const auto event = InteractionFailure::fromJson(fixture["d"]);
    REQUIRE(event.id == Snowflake(334455667788990011ULL));
    REQUIRE(event.nonce.hasValue());
    REQUIRE(event.nonce.get() == "opaque_nonce_failure_1");
    REQUIRE(event.reasonCode == 2);
}

TEST_CASE("InteractionFailure preserves unknown reason codes with undefined nonce", "[interaction-events]")
{
    const auto fixture = nlohmann::json::parse(R"({
        "t": "INTERACTION_FAILURE",
        "d": {
            "id": "334455667788990011",
            "reason_code": 9999
        }
    })");

    const auto event = InteractionFailure::fromJson(fixture["d"]);
    REQUIRE(event.id == Snowflake(334455667788990011ULL));
    REQUIRE(event.nonce.isUndefined());
    REQUIRE(event.reasonCode == 9999);
}

TEST_CASE("GatewayEvent maps APPLICATION_COMMAND_AUTOCOMPLETE_RESPONSE", "[interaction-events]")
{
    REQUIRE(parseGatewayEvent("APPLICATION_COMMAND_AUTOCOMPLETE_RESPONSE") == GatewayEvent::APPLICATION_COMMAND_AUTOCOMPLETE_RESPONSE);
}
