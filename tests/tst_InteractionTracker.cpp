#include <catch2/catch_test_macros.hpp>

#include "UI/Chat/InteractionTracker.hpp"

#include <string>

using namespace Styx;
using namespace Styx::UI;

TEST_CASE("Begin then success by nonce", "[interactions]")
{
    InteractionTracker tracker;
    const Core::Snowflake messageId(101);
    const std::string nonce = "nonce-1";
    const std::string path = "0.0";

    REQUIRE(tracker.begin(nonce, messageId, path));
    REQUIRE(tracker.pendingPath(messageId) == path);
    REQUIRE_FALSE(tracker.hasFailed(messageId));

    const auto completed = tracker.succeeded(Core::Snowflake(999), nonce);
    REQUIRE(completed.has_value());
    REQUIRE(*completed == messageId);
    REQUIRE(tracker.pendingPath(messageId).empty());
    REQUIRE_FALSE(tracker.hasFailed(messageId));
}

TEST_CASE("Success with empty nonce matched through interaction id", "[interactions]")
{
    InteractionTracker tracker;
    const Core::Snowflake messageId(102);
    const Core::Snowflake interactionId(501);
    const std::string nonce = "nonce-2";

    REQUIRE(tracker.begin(nonce, messageId, "0.1"));
    tracker.created(interactionId, nonce);

    const auto completed = tracker.succeeded(interactionId, "");
    REQUIRE(completed.has_value());
    REQUIRE(*completed == messageId);
    REQUIRE(tracker.pendingPath(messageId).empty());
    REQUIRE_FALSE(tracker.hasFailed(messageId));
}

TEST_CASE("Failure by id after created and new begin clears failure", "[interactions]")
{
    InteractionTracker tracker;
    const Core::Snowflake messageId(103);
    const Core::Snowflake interactionId(502);

    REQUIRE(tracker.begin("nonce-3a", messageId, "1.0"));
    tracker.created(interactionId, "nonce-3a");

    const auto failedMessage = tracker.failed(interactionId, "");
    REQUIRE(failedMessage.has_value());
    REQUIRE(*failedMessage == messageId);
    REQUIRE(tracker.hasFailed(messageId));
    REQUIRE(tracker.pendingPath(messageId).empty());

    REQUIRE(tracker.begin("nonce-3b", messageId, "1.1"));
    REQUIRE_FALSE(tracker.hasFailed(messageId));
    REQUIRE(tracker.pendingPath(messageId) == "1.1");
}

TEST_CASE("Second begin on pending message rejected while another message succeeds", "[interactions]")
{
    InteractionTracker tracker;
    const Core::Snowflake message1(104);
    const Core::Snowflake message2(105);

    REQUIRE_FALSE(tracker.begin("", message1, "0.0"));
    REQUIRE_FALSE(tracker.begin("nonce-inv", Core::Snowflake::Invalid, "0.0"));

    REQUIRE(tracker.begin("nonce-4a", message1, "0.0"));
    REQUIRE_FALSE(tracker.begin("nonce-4b", message1, "0.1"));
    REQUIRE(tracker.pendingPath(message1) == "0.0");

    REQUIRE(tracker.begin("nonce-4c", message2, "2.0"));
    REQUIRE(tracker.pendingPath(message2) == "2.0");
}

TEST_CASE("Rejected nonce marks failed, unknown events do nothing, and clear resets state", "[interactions]")
{
    InteractionTracker tracker;
    const Core::Snowflake message1(106);
    const Core::Snowflake message2(107);
    const Core::Snowflake unknownId(888);

    REQUIRE_FALSE(tracker.succeeded(unknownId, "unknown-nonce").has_value());
    REQUIRE_FALSE(tracker.failed(unknownId, "").has_value());
    REQUIRE_FALSE(tracker.rejected("unknown-nonce").has_value());

    tracker.created(unknownId, "unknown-nonce");

    REQUIRE(tracker.begin("nonce-5a", message1, "0.0"));
    const auto rejectedMessage = tracker.rejected("nonce-5a");
    REQUIRE(rejectedMessage.has_value());
    REQUIRE(*rejectedMessage == message1);
    REQUIRE(tracker.hasFailed(message1));
    REQUIRE(tracker.pendingPath(message1).empty());

    REQUIRE(tracker.begin("nonce-5b", message2, "0.1"));
    REQUIRE(tracker.pendingPath(message2) == "0.1");

    tracker.clear();
    REQUIRE(tracker.pendingPath(message2).empty());
    REQUIRE_FALSE(tracker.hasFailed(message1));
    REQUIRE_FALSE(tracker.hasFailed(message2));
}
