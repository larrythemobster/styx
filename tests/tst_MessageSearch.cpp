#include "Discord/MessageSearch.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace Styx::Discord;
using Styx::Core::Snowflake;

using Parameters = std::vector<std::pair<std::string, std::string>>;

static MessageSearchQuery everyFilter()
{
    MessageSearchQuery query;
    query.content = "cats";
    query.authorIds = {Snowflake(1), Snowflake(2)};
    query.mentions = {Snowflake(3)};
    query.channelIds = {Snowflake(4)};
    query.has = {"image", "-link"};
    query.pinned = true;
    query.before = Snowflake(500);
    query.after = Snowflake(100);
    query.offset = 25;
    query.oldestFirst = true;
    return query;
}

TEST_CASE("A bare search only sorts newest first", "[search]")
{
    const Parameters expected = {
        {"sort_by", "timestamp"},
        {"sort_order", "desc"}
    };
    REQUIRE(searchParameters(MessageSearchQuery{}, true) == expected);
}

TEST_CASE("Every filter is sent in order, arrays as repeated keys", "[search]")
{
    const MessageSearchQuery query = everyFilter();
    const Parameters expected = {
        {"content", "cats"},
        {"author_id", "1"},
        {"author_id", "2"},
        {"mentions", "3"},
        {"channel_id", "4"},
        {"has", "image"},
        {"has", "-link"},
        {"pinned", "true"},
        {"max_id", "500"},
        {"min_id", "100"},
        {"offset", "25"},
        {"sort_by", "timestamp"},
        {"sort_order", "asc"}
    };
    REQUIRE(searchParameters(query, true) == expected);
}

TEST_CASE("DM searches drop the channel filter", "[search]")
{
    const MessageSearchQuery query = everyFilter();
    const auto serverParams = searchParameters(query, true);
    const auto dmParams = searchParameters(query, false);

    REQUIRE(std::none_of(dmParams.begin(), dmParams.end(), [](const auto &pair) {
        return pair.first == "channel_id";
    }));
    REQUIRE(dmParams.size() == serverParams.size() - 1);
}

TEST_CASE("Offsets are capped where Discord stops", "[search]")
{
    MessageSearchQuery query;
    query.offset = 20000;
    query.pinned = false;

    const auto params = searchParameters(query, true);
    REQUIRE(std::find(params.begin(), params.end(), std::pair<std::string, std::string>{"offset", "9975"}) != params.end());
    REQUIRE(std::find(params.begin(), params.end(), std::pair<std::string, std::string>{"pinned", "false"}) != params.end());
}

TEST_CASE("A 202 asks to wait for the index", "[search]")
{
    const auto res1 = parseSearchResponse(202, R"({"message":"Index not yet available. Try again later","code":110000,"documents_indexed":0,"retry_after":2})");
    REQUIRE(res1.indexNotReady);
    REQUIRE(res1.retryAfterSeconds == 2);
    REQUIRE(res1.messages.empty());

    const auto res2 = parseSearchResponse(202, R"({"retry_after":0})");
    REQUIRE(res2.indexNotReady);
    REQUIRE(res2.retryAfterSeconds == 1);

    const auto res3 = parseSearchResponse(202, "");
    REQUIRE(res3.indexNotReady);
    REQUIRE(res3.retryAfterSeconds == 1);
}

TEST_CASE("Results are unwrapped from their groups", "[search]")
{
    const std::string body = R"({"total_results":2,"messages":[[{"id":"10","channel_id":"5","content":"one","author":{"id":"1","username":"a"}}],[{"id":"11","channel_id":"5","content":"before","author":{"id":"1","username":"a"}},{"id":"12","channel_id":"5","content":"two","hit":true,"author":{"id":"1","username":"a"}}]],"threads":[{"id":"7","type":11,"name":"t"}]})";

    const auto result = parseSearchResponse(200, body);
    REQUIRE_FALSE(result.indexNotReady);
    REQUIRE(result.totalResults == 2);
    REQUIRE(result.messages.size() == 2);
    REQUIRE(result.messages[0].content.get() == "one");
    REQUIRE(result.messages[1].id.get() == Snowflake(12));
    REQUIRE(result.threads.size() == 1);
    REQUIRE(result.threads[0].id.get() == Snowflake(7));
}

TEST_CASE("Malformed bodies give an empty result", "[search]")
{
    const auto res1 = parseSearchResponse(200, "not json");
    REQUIRE_FALSE(res1.indexNotReady);
    REQUIRE(res1.totalResults == 0);
    REQUIRE(res1.messages.empty());

    const auto res2 = parseSearchResponse(200, R"({"messages":"x"})");
    REQUIRE_FALSE(res2.indexNotReady);
    REQUIRE(res2.totalResults == 0);
    REQUIRE(res2.messages.empty());
}
