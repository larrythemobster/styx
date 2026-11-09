#include "Core/CommandIndexCache.hpp"
#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/ApplicationCommands.hpp"
#include "Platform/Signal.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <utility>
#include <vector>

using Styx::Core::CommandIndexCache;
using Styx::Core::Result;
using Styx::Core::Snowflake;
using Scope = CommandIndexCache::Scope;
using ScopeKind = CommandIndexCache::ScopeKind;

namespace {

Scope guildScope(Snowflake id)
{
    return {ScopeKind::Guild, id};
}

Scope channelScope(Snowflake id)
{
    return {ScopeKind::Channel, id};
}

Scope userScope(Snowflake id = Snowflake::Invalid)
{
    return {ScopeKind::User, id};
}

struct FakeFetch
{
    struct Pending
    {
        Scope scope;
        CommandIndexCache::FetchCallback callback;
    };

    std::vector<Pending> requests;

    CommandIndexCache::Fetch fetcher()
    {
        return [this](Scope scope, CommandIndexCache::FetchCallback callback) {
            requests.push_back({scope, std::move(callback)});
        };
    }

    void complete(std::size_t index, Snowflake version)
    {
        Styx::Discord::ApplicationCommandIndex commandIndex;
        commandIndex.version = version;
        requests.at(index).callback(Result<Styx::Discord::ApplicationCommandIndex>::makeOk(std::move(commandIndex)));
    }

    void fail(std::size_t index)
    {
        requests.at(index).callback(Result<Styx::Discord::ApplicationCommandIndex>::makeError("Failed to fetch"));
    }
};

struct Fixture
{
    FakeFetch fake;
    CommandIndexCache cache;
    std::vector<Scope> readyScopes;
    Styx::Platform::Connection connection;

    Fixture()
        : cache(fake.fetcher())
        , connection(cache.indexReady.connect([this](Scope scope) {
            readyScopes.push_back(scope);
        }))
    {
    }

    explicit Fixture(CommandIndexCache::Fetch fetch)
        : cache(std::move(fetch))
        , connection(cache.indexReady.connect([this](Scope scope) {
            readyScopes.push_back(scope);
        }))
    {
    }
};

} // namespace

TEST_CASE("First request starts fetch and completion caches index", "[commands]")
{
    Fixture f;
    const auto scope = guildScope(Snowflake(1));

    REQUIRE(f.cache.request(scope) == nullptr);
    REQUIRE(f.fake.requests.size() == 1);

    REQUIRE(f.cache.request(scope) == nullptr);
    REQUIRE(f.fake.requests.size() == 1);

    f.fake.complete(0, Snowflake(100));

    const auto *index = f.cache.request(scope);
    REQUIRE(index != nullptr);
    REQUIRE(index->version == Snowflake(100));
    REQUIRE(f.readyScopes.size() == 1);
    REQUIRE(f.readyScopes.front() == scope);
}

TEST_CASE("Different scopes fetch separately and user scopes share one fetch", "[commands]")
{
    Fixture f;
    const auto guild1 = guildScope(Snowflake(1));
    const auto guild2 = guildScope(Snowflake(2));
    const auto channel1 = channelScope(Snowflake(10));
    const auto user1 = userScope(Snowflake(100));
    const auto user2 = userScope(Snowflake(200));

    REQUIRE(f.cache.request(guild1) == nullptr);
    REQUIRE(f.cache.request(guild2) == nullptr);
    REQUIRE(f.cache.request(channel1) == nullptr);
    REQUIRE(f.cache.request(user1) == nullptr);
    REQUIRE(f.fake.requests.size() == 4);

    REQUIRE(f.cache.request(user2) == nullptr);
    REQUIRE(f.fake.requests.size() == 4);

    REQUIRE(f.fake.requests[0].scope == guild1);
    REQUIRE(f.fake.requests[1].scope == guild2);
    REQUIRE(f.fake.requests[2].scope == channel1);
    REQUIRE(f.fake.requests[3].scope == userScope(Snowflake::Invalid));
}

TEST_CASE("Failed fetch caches nothing and next request fetches again", "[commands]")
{
    Fixture f;
    const auto scope = guildScope(Snowflake(1));

    REQUIRE(f.cache.request(scope) == nullptr);
    REQUIRE(f.fake.requests.size() == 1);

    f.fake.fail(0);
    REQUIRE(f.readyScopes.empty());

    REQUIRE(f.cache.request(scope) == nullptr);
    REQUIRE(f.fake.requests.size() == 2);
}

TEST_CASE("Clear while in flight discards completion and refetches", "[commands]")
{
    Fixture f;
    const auto scope = guildScope(Snowflake(1));

    REQUIRE(f.cache.request(scope) == nullptr);
    REQUIRE(f.fake.requests.size() == 1);

    f.cache.clear();

    f.fake.complete(0, Snowflake(100));
    REQUIRE(f.readyScopes.empty());

    REQUIRE(f.cache.request(scope) == nullptr);
    REQUIRE(f.fake.requests.size() == 2);
}

TEST_CASE("Synchronous fetch returns index immediately from request", "[commands]")
{
    const auto scope = guildScope(Snowflake(1));
    Fixture f([](Scope, CommandIndexCache::FetchCallback callback) {
        Styx::Discord::ApplicationCommandIndex index;
        index.version = Snowflake(42);
        callback(Result<Styx::Discord::ApplicationCommandIndex>::makeOk(std::move(index)));
    });

    const auto *index = f.cache.request(scope);
    REQUIRE(index != nullptr);
    REQUIRE(index->version == Snowflake(42));
    REQUIRE(f.readyScopes.size() == 1);
    REQUIRE(f.readyScopes.front() == scope);
}
