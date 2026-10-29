// Ported from ouwou/acheron upstream/video-streams (commit 71b4907) tests/tst_Calls.cpp
// Tests for private (DM and group DM) call signalling: the three call events, opcode 13, and
// the per-session record of calls. The store's rules are the official client's CallStore
// (docs/video-re/re-dm-calls.md), including what it keeps after a call ends.

#include "Core/Calls/CallStore.hpp"
#include "Discord/Events.hpp"
#include "Discord/Outbound.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>

#include <vector>

using namespace Styx;
using Core::CallStore;
using Core::Snowflake;

namespace {

const Snowflake CHANNEL(5000);

template <typename Event>
Event parseEvent(const char *json)
{
    return Event::fromJson(nlohmann::json::parse(json));
}

Discord::CallCreate ongoingCall()
{
    return parseEvent<Discord::CallCreate>(R"({"channel_id":"5000","message_id":"6000","region":"rotterdam","ongoing_rings":{},"voice_states":[{"user_id":"111","channel_id":"5000","session_id":"abc","deaf":false,"mute":false,"self_deaf":false,"self_mute":true,"self_video":true,"self_stream":true,"suppress":false}]})");
}

} // namespace

TEST_CASE("Calls: connectPayload", "[calls]")
{
    Discord::ConnectCall msg;
    msg.channelId = CHANNEL;
    CHECK(msg.toJson().toBytes() == std::string(R"({"op":13,"d":{"channel_id":"5000"}})"));
}

TEST_CASE("Calls: createParsesCallAndItsVoiceStates", "[calls]")
{
    const Discord::CallCreate call = ongoingCall();

    CHECK(uint64_t(call.channelId.get()) == uint64_t(5000));
    CHECK(uint64_t(call.messageId.get()) == uint64_t(6000));
    CHECK(call.region.value_or(std::string()) == std::string("rotterdam"));
    CHECK(call.ongoingRings.empty());

    REQUIRE(call.voiceStates.hasValue());
    REQUIRE(call.voiceStates->size() == 1);
    const Discord::VoiceState &state = call.voiceStates->front();
    CHECK(uint64_t(state.userId.get()) == uint64_t(111));
    CHECK(uint64_t(state.channelId.get()) == uint64_t(5000));
    CHECK(state.sessionId.get() == std::string("abc"));
    CHECK(!state.guildId.hasValue());
    CHECK(state.selfMute.get() == true);
    CHECK(state.selfVideo.get() == true);
    CHECK(state.selfStream.valueOr(false) == true);
}

TEST_CASE("Calls: ringsMapRungUserToRinger", "[calls]")
{
    const auto update = parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","region":"rotterdam","ongoing_rings":{"222":"111","333":null}})");

    CHECK(update.ongoingRings.size() == 2);
    CHECK(uint64_t(update.ongoingRings.at(Snowflake(222))) == uint64_t(111));
    CHECK(update.ongoingRings.contains(Snowflake(333)));
    CHECK(!update.ongoingRings.at(Snowflake(333)).isValid());
}

TEST_CASE("Calls: nullRegionIsNoRegion", "[calls]")
{
    CHECK(!parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","region":null,"ongoing_rings":{}})").region.has_value());
    CHECK(!parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","ongoing_rings":{}})").region.has_value());
}

TEST_CASE("Calls: selectingADmAsksOnce", "[calls]")
{
    CallStore store;

    CHECK(store.shouldConnect(CHANNEL, false));
    CHECK(!store.shouldConnect(CHANNEL, false));
    CHECK(!store.isActive(CHANNEL));

    // Nor after the gateway has answered, whether or not a call was there.
    store.handleCallCreate(ongoingCall());
    CHECK(!store.shouldConnect(CHANNEL, false));
}

TEST_CASE("Calls: aFreshSessionAsksAgain", "[calls]")
{
    CallStore store;
    store.handleCallCreate(ongoingCall());

    CHECK(store.shouldConnect(CHANNEL, true));
    CHECK(store.shouldConnect(CHANNEL, true));

    store.reset();
    CHECK(store.shouldConnect(CHANNEL, false));
}

TEST_CASE("Calls: createMakesTheCallActive", "[calls]")
{
    CallStore store;
    std::vector<Snowflake> changed;
    Platform::Connection conn = store.callChanged.connect([&changed](Snowflake id) {
        changed.push_back(id);
    });

    store.handleCallCreate(ongoingCall());

    CHECK(store.isActive(CHANNEL));
    const auto call = store.call(CHANNEL);
    REQUIRE(call.has_value());
    CHECK(uint64_t(call->messageId) == uint64_t(6000));
    CHECK(call->region.value_or(std::string()) == "rotterdam");
    REQUIRE(changed.size() == 1);
    CHECK(uint64_t(changed.front()) == uint64_t(5000));
}

TEST_CASE("Calls: aCallWithoutRegionIsNotActive", "[calls]")
{
    CallStore store;
    store.handleCallCreate(parseEvent<Discord::CallCreate>(R"({"channel_id":"5000","message_id":"6000","region":null,"ongoing_rings":{"222":"111"}})"));

    CHECK(store.call(CHANNEL).has_value());
    CHECK(!store.isActive(CHANNEL));

    store.handleCallUpdate(parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","region":"rotterdam","ongoing_rings":{"222":"111"}})"));
    CHECK(store.isActive(CHANNEL));
}

TEST_CASE("Calls: updateKeepsUnavailability", "[calls]")
{
    CallStore store;
    store.handleCallCreate(ongoingCall());
    store.handleCallDelete(parseEvent<Discord::CallDelete>(R"({"channel_id":"5000","unavailable":true})"));

    store.handleCallUpdate(parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","region":"frankfurt","ongoing_rings":{}})"));

    CHECK(store.call(CHANNEL)->region.value_or(std::string()) == "frankfurt");
    CHECK(store.call(CHANNEL)->unavailable);
    CHECK(!store.isActive(CHANNEL));

    // Only a new CALL_CREATE brings an unavailable call back.
    store.handleCallCreate(ongoingCall());
    CHECK(store.isActive(CHANNEL));
}

TEST_CASE("Calls: updateForAnUnknownCallRecordsIt", "[calls]")
{
    CallStore store;
    store.handleCallUpdate(parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","region":"rotterdam","ongoing_rings":{}})"));

    CHECK(store.isActive(CHANNEL));
}

TEST_CASE("Calls: deleteLeavesAKnownButEndedCall", "[calls]")
{
    CallStore store;
    store.handleCallCreate(ongoingCall());
    std::vector<Snowflake> changed;
    Platform::Connection conn = store.callChanged.connect([&changed](Snowflake id) {
        changed.push_back(id);
    });

    store.handleCallDelete(parseEvent<Discord::CallDelete>(R"({"channel_id":"5000"})"));

    CHECK(!store.isActive(CHANNEL));
    REQUIRE(changed.size() == 1);

    const auto call = store.call(CHANNEL);
    REQUIRE(call.has_value());
    CHECK(!call->region.has_value());
    CHECK(!call->messageId.isValid());
    CHECK(call->ongoingRings.empty());

    // The ended call is still on record, so looking at the DM again does not ask the gateway.
    CHECK(!store.shouldConnect(CHANNEL, false));
}

TEST_CASE("Calls: unavailableKeepsTheCallButNotActive", "[calls]")
{
    CallStore store;
    store.handleCallCreate(ongoingCall());

    store.handleCallDelete(parseEvent<Discord::CallDelete>(R"({"channel_id":"5000","unavailable":true})"));

    const auto call = store.call(CHANNEL);
    REQUIRE(call.has_value());
    CHECK(call->unavailable);
    CHECK(call->region.value_or(std::string()) == "rotterdam");
    CHECK(uint64_t(call->messageId) == uint64_t(6000));
    CHECK(!store.isActive(CHANNEL));
}

TEST_CASE("Calls: unavailableForAnUnknownCallIsNotActive", "[calls]")
{
    CallStore store;
    store.handleCallDelete(parseEvent<Discord::CallDelete>(R"({"channel_id":"5000","unavailable":true})"));

    REQUIRE(store.call(CHANNEL).has_value());
    CHECK(store.call(CHANNEL)->unavailable);
    CHECK(!store.isActive(CHANNEL));
}

TEST_CASE("Calls: ringingIsOnlyReportedForActiveCalls", "[calls]")
{
    CallStore store;
    store.handleCallCreate(parseEvent<Discord::CallCreate>(R"({"channel_id":"5000","message_id":"6000","region":"rotterdam","ongoing_rings":{"222":"111"}})"));

    CHECK(store.isRinging(CHANNEL, Snowflake(222)));
    CHECK(!store.isRinging(CHANNEL, Snowflake(111)));
    CHECK(!store.isRinging(Snowflake(9999), Snowflake(222)));

    store.handleCallDelete(parseEvent<Discord::CallDelete>(R"({"channel_id":"5000"})"));
    CHECK(!store.isRinging(CHANNEL, Snowflake(222)));
}

TEST_CASE("Calls: forgetAndResetDropCalls", "[calls]")
{
    CallStore store;
    store.handleCallCreate(ongoingCall());
    std::vector<Snowflake> changed;
    Platform::Connection conn = store.callChanged.connect([&changed](Snowflake id) {
        changed.push_back(id);
    });

    store.forget(CHANNEL);
    CHECK(!store.call(CHANNEL).has_value());
    CHECK(changed.size() == 1);

    store.forget(CHANNEL);
    CHECK(changed.size() == 1);

    store.handleCallCreate(ongoingCall());
    store.reset();
    CHECK(!store.call(CHANNEL).has_value());
}
