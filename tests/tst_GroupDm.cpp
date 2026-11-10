#include "Discord/GroupDm.hpp"
#include "Discord/HttpClient.hpp"

#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace Styx::Discord;
using Styx::Core::Snowflake;
using Json = nlohmann::json;

namespace {

HttpResponse response(bool success, int status, std::string body = {})
{
    return {status, std::move(body), {}, success};
}

Json group()
{
    return Json::parse(R"({
        "id":"200", "type":3, "name":"Secret Club", "icon":null,
        "owner_id":"100", "recipients":[
            {"id":"101","username":"alice"}, {"id":"102","username":"bob"}
        ]
    })");
}

} // namespace

TEST_CASE("Group DM creation distinguishes single-recipient shells from private channels", "[group_dm]")
{
    struct Case { std::vector<Snowflake> recipients; const char *wire; };
    for (const auto &item : {
            Case{{}, R"({"recipients":[]})"},
            Case{{Snowflake(100)}, R"({"recipient_id":"100"})"},
            Case{{Snowflake(100), Snowflake(200), Snowflake(300)}, R"({"recipients":["100","200","300"]})"}}) {
        INFO(item.wire);
        const auto result = groupDmCreateBody(item.recipients);
        REQUIRE(result.success());
        CHECK(*result.value == Json::parse(item.wire));
    }
}

TEST_CASE("Group DM creation rejects invalid and duplicate recipients anywhere in the selection", "[group_dm]")
{
    for (const auto &ids : {
            std::vector<Snowflake>{Snowflake::Invalid}, {Snowflake(0)},
            {Snowflake(100), Snowflake(0)}, {Snowflake(100), Snowflake(100)},
            {Snowflake(100), Snowflake(200), Snowflake(100)}}) {
        CHECK_FALSE(groupDmCreateBody(ids).success());
    }
}

TEST_CASE("Group DM edits omit unchanged fields and distinguish icon replacement from removal", "[group_dm]")
{
    GroupDmEdit edit;
    CHECK(Json::parse(edit.toJson().toBytes()) == Json::object());
    edit.name = "Study Group";
    CHECK(Json::parse(edit.toJson().toBytes()) == Json::parse(R"({"name":"Study Group"})"));
    edit.icon = nullptr;
    CHECK(Json::parse(edit.toJson().toBytes()) == Json::parse(R"({"name":"Study Group","icon":null})"));
    edit.icon = "data:image/png;base64,AQID";
    CHECK(Json::parse(edit.toJson().toBytes()) == Json::parse(R"({"name":"Study Group","icon":"data:image/png;base64,AQID"})"));
    GroupDmEdit iconOnly;
    iconOnly.icon = "data:image/png;base64,AQID";
    CHECK(Json::parse(iconOnly.toJson().toBytes()) == Json::parse(R"({"icon":"data:image/png;base64,AQID"})"));
    iconOnly.icon = nullptr;
    CHECK(Json::parse(iconOnly.toJson().toBytes()) == Json::parse(R"({"icon":null})"));
}

TEST_CASE("Group DM responses reject malformed containers and missing required fields", "[group_dm]")
{
    for (const auto *body : {"{broken", "[]", "null", "{}"})
        CHECK_FALSE(groupDmChannelResult(response(true, 200, body)).success());
    for (const auto *key : {"id", "type"}) {
        auto payload = group();
        payload.erase(key);
        CHECK_FALSE(groupDmChannelResult(response(true, 200, payload.dump())).success());
    }
}

TEST_CASE("Group DM responses reject invalid IDs, wrong channel types and malformed optional data", "[group_dm]")
{
    const auto patches = Json::parse(R"([
        {"id":200}, {"id":"0"}, {"id":"invalid"}, {"id":"18446744073709551615"},
        {"type":1}, {"type":0}, {"type":"3"}, {"type":3.0}, {"type":4294967299},
        {"recipients":"invalid"}, {"recipients":null}, {"recipients":["invalid"]},
        {"recipients":[{"username":"alice"}]}, {"recipients":[{"id":"0"}]},
        {"recipients":[{"id":"invalid"}]}, {"recipients":[{"id":101}]},
        {"recipients":[{"id":"101"},{"id":"101"}]},
        {"owner_id":100}, {"owner_id":"0"}, {"owner_id":"invalid"}, {"owner_id":null},
        {"name":12345}, {"icon":true}
    ])");
    for (const auto &patch : patches) {
        INFO(patch.dump());
        auto payload = group();
        payload.update(patch);
        CHECK_FALSE(groupDmChannelResult(response(true, 200, payload.dump())).success());
    }
    CHECK_FALSE(groupDmChannelResult(response(true, 200, group().dump()), Snowflake(999)).success());
}

TEST_CASE("Group DM snapshots preserve recipients, ownership and omitted versus cleared metadata", "[group_dm]")
{
    const auto result = groupDmChannelResult(response(true, 200, group().dump()), Snowflake(200));
    REQUIRE(result.success());
    CHECK(result.value->id.get() == Snowflake(200));
    CHECK(result.value->name.get() == "Secret Club");
    CHECK(result.value->icon.isNull());
    CHECK(result.value->ownerId.get() == Snowflake(100));
    REQUIRE(result.value->recipients.hasValue());
    REQUIRE(result.value->recipients->size() == 2);
    CHECK(result.value->recipients.get()[0].id.get() == Snowflake(101));
    CHECK(result.value->recipients.get()[1].id.get() == Snowflake(102));

    const auto minimal = groupDmChannelResult(response(true, 200, R"({"id":"500","type":3})"));
    REQUIRE(minimal.success());
    CHECK(minimal.value->id.get() == Snowflake(500));
    CHECK(minimal.value->recipients.isUndefined());
    CHECK(minimal.value->ownerId.isUndefined());
    CHECK(minimal.value->name.isUndefined());
    CHECK(minimal.value->icon.isUndefined());
    const auto empty = groupDmChannelResult(response(true, 200, R"({"id":"501","type":3,"recipients":[]})"));
    REQUIRE(empty.success());
    REQUIRE(empty.value->recipients.hasValue());
    CHECK(empty.value->recipients->empty());
}

TEST_CASE("Recipient additions distinguish admission from a newly created group", "[group_dm]")
{
    const auto admitted = addDmRecipientResult(response(true, 204));
    REQUIRE(admitted.success());
    CHECK_FALSE(admitted.value->has_value());
    const auto converted = addDmRecipientResult(response(true, 200, group().dump()));
    REQUIRE(converted.success());
    REQUIRE(converted.value->has_value());
    CHECK(converted.value->value().id.get() == Snowflake(200));
    CHECK(converted.value->value().type.get() == ChannelType::GROUP_DM);
    for (int status : {201, 202})
        CHECK_FALSE(addDmRecipientResult(response(true, status, group().dump())).success());
}

TEST_CASE("Group DM channel and removal responses enforce their distinct status contracts", "[group_dm]")
{
    for (int status : {201, 204})
        CHECK_FALSE(groupDmChannelResult(response(true, status, group().dump())).success());
    CHECK(groupDmActionResult(response(true, 204)).success());
    for (int status : {200, 201})
        CHECK_FALSE(groupDmActionResult(response(true, status, group().dump())).success());
}

TEST_CASE("Group DM operations preserve Discord and transport errors", "[group_dm]")
{
    const auto check = [](const auto &result, int code, const std::string &message) {
        CHECK_FALSE(result.success());
        CHECK(result.code == code);
        CHECK(result.error == message);
    };
    auto error = response(false, 403, R"({"code":50013,"message":"Missing Permissions"})");
    check(groupDmChannelResult(error), 50013, "Missing Permissions");
    check(addDmRecipientResult(error), 50013, "Missing Permissions");
    check(groupDmActionResult(error), 50013, "Missing Permissions");
    error = response(false, 0);
    error.error = "Connection refused";
    check(groupDmChannelResult(error), 0, "Connection refused");
    check(addDmRecipientResult(error), 0, "Connection refused");
    check(groupDmActionResult(error), 0, "Connection refused");
}
