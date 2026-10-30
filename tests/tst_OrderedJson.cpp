// Tests for Core::OrderedJson, the writer behind every gateway payload and the
// X-Super-Properties / X-Context-Properties headers. Expectations are the bytes JSON.stringify
// produces in the official client, so a drift here is a wire fingerprint, not a cosmetic diff.

#include "Core/JsonUtils.hpp"
#include "Core/OrderedJson.hpp"
#include "Discord/ContextProperties.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <cmath>

using namespace Styx;
using Core::OrderedJson;

TEST_CASE("OrderedJson: testEmpty", "[ordered-json]")
{
    REQUIRE(OrderedJson().toBytes() == std::string("{}"));
    REQUIRE(OrderedJson::Array().toBytes() == std::string("[]"));
}

TEST_CASE("OrderedJson: testInsertionOrder", "[ordered-json]")
{
    OrderedJson obj;
    obj.insert("zeta", 1);
    obj.insert("alpha", 2);
    obj.insert("mid", 3);
    REQUIRE(obj.toBytes() == std::string(R"({"zeta":1,"alpha":2,"mid":3})"));
}

TEST_CASE("OrderedJson: testScalars", "[ordered-json]")
{
    OrderedJson obj;
    obj.insert("t", true);
    obj.insert("f", false);
    obj.insert("n", nullptr);
    obj.insert("s", "text");
    REQUIRE(obj.toBytes() == std::string(R"({"t":true,"f":false,"n":null,"s":"text"})"));
}

TEST_CASE("OrderedJson: testNumbers", "[ordered-json]")
{
    OrderedJson obj;
    obj.insert("int", 42);
    obj.insert("neg", -7);
    obj.insert("wholeDouble", 2.0);
    obj.insert("negZero", -0.0);
    obj.insert("fraction", 1.5);
    obj.insert("big", static_cast<int64_t>(1700000000000LL));
    obj.insert("nan", std::nan(""));
    obj.insert("inf", INFINITY);
    REQUIRE(obj.toBytes() == std::string(R"({"int":42,"neg":-7,"wholeDouble":2,"negZero":0,"fraction":1.5,)"
                        R"("big":1700000000000,"nan":null,"inf":null})"));
}

TEST_CASE("OrderedJson: testStringEscaping", "[ordered-json]")
{
    OrderedJson obj;
    obj.insert("q", "say \"hi\"");
    obj.insert("bs", "a\\b");
    obj.insert("ctl", std::string("tab\tnl\ncr\rbs\bff\f"));
    obj.insert("low", std::string("\x01\x1f", 2));
    obj.insert("slash", "a/b");
    obj.insert("uni", std::string("caf\xC3\xA9 \xF0\x9F\x98\x80"));
    obj.insert(std::string("k\xC3\xA9y"), 1);
    // Keep the escaped raw string outside the assertion macro for Visual Studio.
    const std::string expected = std::string(R"({"q":"say \"hi\"","bs":"a\\b","ctl":"tab\tnl\ncr\rbs\bff\f",)"
                                           R"("low":"\u0001\u001f","slash":"a/b","uni":"caf)") +
                                std::string("\xC3\xA9 \xF0\x9F\x98\x80") + std::string(R"(","k)") + std::string("\xC3\xA9") + std::string(R"(y":1})");
    REQUIRE(obj.toBytes() == expected);
}

TEST_CASE("OrderedJson: testNesting", "[ordered-json]")
{
    OrderedJson inner;
    inner.insert("b", 1);
    inner.insert("a", 2);

    OrderedJson::Array pairs;
    pairs.append(OrderedJson::Array().append(0).append(99));
    pairs.append(OrderedJson::Array().append(100).append(199));

    OrderedJson::Array objects;
    objects.append(inner);

    OrderedJson obj;
    obj.insert("inner", inner);
    obj.insert("pairs", pairs);
    obj.insert("objects", objects);
    obj.insert("empty", OrderedJson());
    REQUIRE(obj.toBytes() == std::string(R"({"inner":{"b":1,"a":2},"pairs":[[0,99],[100,199]],"objects":[{"b":1,"a":2}],"empty":{}})"));
}

TEST_CASE("OrderedJson: native array values", "[ordered-json]")
{
    OrderedJson obj;
    obj.insert("activities", nlohmann::json::array());
    obj.insert("mixed", nlohmann::json::array({ 1, "two", nullptr, true }));
    REQUIRE(obj.toBytes() == std::string(R"({"activities":[],"mixed":[1,"two",null,true]})"));
}

TEST_CASE("OrderedJson: sorted native JSON values", "[ordered-json]")
{
    nlohmann::json native = {{"a", std::string("q\"uote\\ \n \xC3\xA9")},
                             {"b", 1234567890123LL}, {"c", 0.1},
                             {"d", nlohmann::json::array({1, 2.5, "x"})},
                             {"e", nullptr}, {"f", false}};
    OrderedJson ordered;
    for (auto it = native.cbegin(); it != native.cend(); ++it)
        ordered.insert(it.key(), it.value());
    REQUIRE(ordered.toBytes() == native.dump());
}

namespace {

struct Inner : Core::JsonUtils::JsonObject
{
    Field<std::string> name;
    Field<int> value;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "name", name);
        insert(obj, "value", value);
        return obj;
    }
};

struct Outer : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::vector<Core::Snowflake>> ids;
    Field<std::string, true> absent;
    Field<std::string, false, true> nulled;
    Field<Inner> inner;
    Field<std::vector<Inner>> inners;
    Field<std::map<Core::Snowflake, Inner>> byId;
    Field<std::vector<uint8_t>> bytes;
    Field<uint32_t> ssrc;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "id", id);
        insert(obj, "ids", ids);
        insert(obj, "absent", absent);
        insert(obj, "nulled", nulled);
        insert(obj, "inner", inner);
        insert(obj, "inners", inners);
        insert(obj, "by_id", byId);
        insert(obj, "bytes", bytes);
        insert(obj, "ssrc", ssrc);
        return obj;
    }
};

} // namespace

TEST_CASE("OrderedJson: testFieldInsert", "[ordered-json]")
{
    Inner one;
    one.name = "one";
    one.value = 1;
    Inner two;
    two.name = "two";
    two.value = 2;

    Outer outer;
    outer.id = Core::Snowflake(1234567890123456789ULL);
    outer.ids = std::vector<Core::Snowflake>{ Core::Snowflake(1), Core::Snowflake(2) };
    outer.nulled = nullptr;
    outer.inner = one;
    outer.inners = std::vector<Inner>{ one, two };
    outer.byId = std::map<Core::Snowflake, Inner>{ { Core::Snowflake(7), two } };
    outer.bytes = std::vector<uint8_t>{ 0, 255 };
    outer.ssrc = 4000000000u;

    REQUIRE(outer.toJson().toBytes() == std::string(R"({"id":"1234567890123456789","ids":["1","2"],"nulled":null,)"
                        R"("inner":{"name":"one","value":1},)"
                        R"("inners":[{"name":"one","value":1},{"name":"two","value":2}],)"
                        R"("by_id":{"7":{"name":"two","value":2}},"bytes":[0,255],"ssrc":4000000000})"));
}

TEST_CASE("OrderedJson: string fields remain strings", "[ordered-json]")
{
    Inner inner;
    inner.name = std::string("caf\xC3\xA9\n");
    inner.value = 0;
    REQUIRE(inner.toJson().toBytes() == std::string("{\"name\":\"caf\xC3\xA9\\n\",\"value\":0}"));

    inner.name = std::string();
    REQUIRE(inner.toJson().toBytes() == std::string(R"({"name":"","value":0})"));
}

TEST_CASE("OrderedJson: testContextProperties", "[ordered-json]")
{
    REQUIRE(Discord::ContextProperties::empty().toHeaderValue() == std::string("e30="));
    REQUIRE(Discord::ContextProperties::location("chat_input").toHeaderValue() == std::string("eyJsb2NhdGlvbiI6ImNoYXRfaW5wdXQifQ=="));

    Discord::ContextProperties invite = Discord::ContextProperties::location("Join Guild");
    invite.add("location_guild_id", "1").add("location_channel_type", 0);
    REQUIRE(invite.toHeaderValue() == std::string("eyJsb2NhdGlvbiI6IkpvaW4gR3VpbGQiLCJsb2NhdGlvbl9ndWlsZF9pZCI6IjEiLCJsb2NhdGlvbl9jaGFubmVsX3R5cGUiOjB9"));
}

