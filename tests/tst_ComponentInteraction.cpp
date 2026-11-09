#include "Discord/ComponentInteraction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace Styx::Discord;

namespace {

ComponentInteractionRequest createBaseRequest()
{
    ComponentInteractionRequest request;
    request.applicationId = Styx::Core::Snowflake(1001);
    request.guildId = Styx::Core::Snowflake(1002);
    request.channelId = Styx::Core::Snowflake(1003);
    request.messageId = Styx::Core::Snowflake(1004);
    request.messageFlags = 64;
    request.componentType = MessageComponentType::BUTTON;
    request.customId = "button_ok";
    request.nonce = "nonce_1";
    return request;
}

} // namespace

TEST_CASE("componentInteractionBody serializes guild button exact whole JSON", "[interaction]")
{
    const auto request = createBaseRequest();
    const auto result = componentInteractionBody(request, "sess_123");
    REQUIRE(result.success());

    const std::string expected =
        R"({"application_id":"1001","channel_id":"1003","data":{"component_type":2,"custom_id":"button_ok"},"guild_id":"1002","message_flags":64,"message_id":"1004","nonce":"nonce_1","session_id":"sess_123","type":3})";

    REQUIRE(*result.value == nlohmann::json::parse(expected));
}

TEST_CASE("componentInteractionBody handles DM select empty selection versus omitted button values", "[interaction]")
{
    auto buttonRequest = createBaseRequest();
    buttonRequest.guildId = Styx::Core::Snowflake::Invalid;
    buttonRequest.values = { "ignored_val" };

    const auto buttonResult = componentInteractionBody(buttonRequest, "sess_dm");
    REQUIRE(buttonResult.success());
    REQUIRE_FALSE((*buttonResult.value).contains("guild_id"));
    REQUIRE_FALSE((*buttonResult.value)["data"].contains("values"));

    auto selectRequest = createBaseRequest();
    selectRequest.guildId = Styx::Core::Snowflake::Invalid;
    selectRequest.componentType = MessageComponentType::STRING_SELECT;
    selectRequest.values.clear();

    const auto selectResult = componentInteractionBody(selectRequest, "sess_dm");
    REQUIRE(selectResult.success());
    REQUIRE_FALSE((*selectResult.value).contains("guild_id"));
    REQUIRE((*selectResult.value)["data"].contains("values"));
    REQUIRE((*selectResult.value)["data"]["values"] == nlohmann::json::array());
}

TEST_CASE("componentInteractionBody handles all select types and preserves custom unicode values, nonce and flags", "[interaction]")
{
    struct SelectTypeCase
    {
        MessageComponentType type;
        int wireType;
        std::vector<std::string> values;
    };

    const SelectTypeCase cases[] = {
        { MessageComponentType::STRING_SELECT, 3, { "opt_café", "val_🚀" } },
        { MessageComponentType::USER_SELECT, 5, { "112233445566778899" } },
        { MessageComponentType::ROLE_SELECT, 6, { "223344556677889900" } },
        { MessageComponentType::MENTIONABLE_SELECT, 7, { "112233445566778899", "223344556677889900" } },
        { MessageComponentType::CHANNEL_SELECT, 8, { "334455667788990011" } },
    };

    for (const auto &tc : cases) {
        auto request = createBaseRequest();
        request.componentType = tc.type;
        request.customId = "sel_⚡_id";
        request.nonce = "nonce_日本語_✨";
        request.messageFlags = 18446744073709551615ULL;
        request.values = tc.values;

        const auto result = componentInteractionBody(request, "sess_unicode");
        REQUIRE(result.success());
        REQUIRE((*result.value)["data"]["component_type"] == tc.wireType);
        REQUIRE((*result.value)["data"]["custom_id"] == "sel_⚡_id");
        REQUIRE((*result.value)["data"]["values"] == tc.values);
        REQUIRE((*result.value)["nonce"] == "nonce_日本語_✨");
        REQUIRE((*result.value)["message_flags"] == 18446744073709551615ULL);
    }
}

TEST_CASE("componentInteractionBody rejects missing required fields and unsupported component types", "[interaction]")
{
    auto req = createBaseRequest();

    auto res = componentInteractionBody(req, "");
    REQUIRE_FALSE(res.success());

    req.nonce.clear();
    res = componentInteractionBody(req, "sess_1");
    REQUIRE_FALSE(res.success());
    req = createBaseRequest();

    req.customId.clear();
    res = componentInteractionBody(req, "sess_1");
    REQUIRE_FALSE(res.success());
    req = createBaseRequest();

    req.applicationId = Styx::Core::Snowflake::Invalid;
    res = componentInteractionBody(req, "sess_1");
    REQUIRE_FALSE(res.success());
    req = createBaseRequest();

    req.channelId = Styx::Core::Snowflake::Invalid;
    res = componentInteractionBody(req, "sess_1");
    REQUIRE_FALSE(res.success());
    req = createBaseRequest();

    req.messageId = Styx::Core::Snowflake::Invalid;
    res = componentInteractionBody(req, "sess_1");
    REQUIRE_FALSE(res.success());
    req = createBaseRequest();

    const MessageComponentType unsupportedTypes[] = {
        MessageComponentType::UNKNOWN,
        MessageComponentType::ACTION_ROW,
        MessageComponentType::SECTION,
        MessageComponentType::TEXT_DISPLAY,
        MessageComponentType::THUMBNAIL,
        MessageComponentType::MEDIA_GALLERY,
        MessageComponentType::FILE,
        MessageComponentType::SEPARATOR,
        MessageComponentType::CONTAINER,
        static_cast<MessageComponentType>(999),
    };

    for (const auto type : unsupportedTypes) {
        req.componentType = type;
        res = componentInteractionBody(req, "sess_1");
        REQUIRE_FALSE(res.success());
    }
}
