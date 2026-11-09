#include "Core/EphemeralMessages.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Enums.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using Styx::Core::EphemeralMessages;
using Styx::Core::Snowflake;
using namespace Styx::Discord;

namespace {

static Message createMessage(Snowflake id, Snowflake channelId, const std::string &content, bool isEphemeral = true)
{
    Message msg;
    msg.id = id;
    msg.channelId = channelId;
    msg.content = content;
    MessageFlags flags;
    if (isEphemeral)
        flags.setFlag(MessageFlag::EPHEMERAL, true);
    msg.flags = flags;
    return msg;
}

} // namespace

TEST_CASE("Ephemeral store isolates messages by channel", "[ephemeral]")
{
    EphemeralMessages store;
    const Snowflake channelA(1001);
    const Snowflake channelB(1002);
    const Snowflake msgA(2001);
    const Snowflake msgB(2002);

    REQUIRE(store.put(createMessage(msgA, channelA, "Channel A ephemeral")));
    REQUIRE(store.put(createMessage(msgB, channelB, "Channel B ephemeral")));

    std::vector<Message> outA;
    store.appendForChannel(channelA, outA);
    REQUIRE(outA.size() == 1);
    REQUIRE(outA.front().id.get() == msgA);
    REQUIRE(outA.front().channelId.get() == channelA);
    REQUIRE(outA.front().content.get() == "Channel A ephemeral");

    std::vector<Message> outB;
    store.appendForChannel(channelB, outB);
    REQUIRE(outB.size() == 1);
    REQUIRE(outB.front().id.get() == msgB);
    REQUIRE(outB.front().channelId.get() == channelB);
    REQUIRE(outB.front().content.get() == "Channel B ephemeral");
}

TEST_CASE("Ephemeral store rejects public messages and invalid IDs", "[ephemeral]")
{
    EphemeralMessages store;
    const Snowflake channelId(1001);
    const Snowflake publicId(2001);

    const auto publicMsg = createMessage(publicId, channelId, "Public message", false);
    REQUIRE_FALSE(store.put(publicMsg));
    REQUIRE(store.find(publicId) == nullptr);

    const auto invalidIdMsg = createMessage(Snowflake::Invalid, channelId, "Invalid ID", true);
    REQUIRE_FALSE(store.put(invalidIdMsg));

    const auto invalidChanMsg = createMessage(Snowflake(2002), Snowflake::Invalid, "Invalid Channel", true);
    REQUIRE_FALSE(store.put(invalidChanMsg));
}

TEST_CASE("Ephemeral store replaces existing message with updated content", "[ephemeral]")
{
    EphemeralMessages store;
    const Snowflake channelId(1001);
    const Snowflake messageId(2001);

    REQUIRE(store.put(createMessage(messageId, channelId, "Initial content")));
    const auto *first = store.find(messageId);
    REQUIRE(first != nullptr);
    REQUIRE(first->content.get() == "Initial content");

    REQUIRE(store.put(createMessage(messageId, channelId, "Updated content")));
    const auto *updated = store.find(messageId);
    REQUIRE(updated != nullptr);
    REQUIRE(updated->content.get() == "Updated content");
}

TEST_CASE("Ephemeral store refuses dismissal for wrong channel or non-stored messages", "[ephemeral]")
{
    EphemeralMessages store;
    const Snowflake channelA(1001);
    const Snowflake channelB(1002);
    const Snowflake messageId(2001);
    const Snowflake unknownId(9999);

    REQUIRE(store.put(createMessage(messageId, channelA, "Ephemeral content")));

    REQUIRE_FALSE(store.dismiss(channelB, messageId));
    REQUIRE_FALSE(store.isDismissed(messageId));
    const auto *stillThere = store.find(messageId);
    REQUIRE(stillThere != nullptr);
    REQUIRE(stillThere->content.get() == "Ephemeral content");

    REQUIRE_FALSE(store.dismiss(channelA, unknownId));
    REQUIRE_FALSE(store.isDismissed(unknownId));
    REQUIRE(store.find(unknownId) == nullptr);
}

TEST_CASE("Dismissal removes message and tombstones ID to reject late put", "[ephemeral]")
{
    EphemeralMessages store;
    const Snowflake channelId(1001);
    const Snowflake messageId(2001);

    REQUIRE(store.put(createMessage(messageId, channelId, "Ephemeral content")));
    REQUIRE(store.find(messageId) != nullptr);

    REQUIRE(store.dismiss(channelId, messageId));
    REQUIRE(store.find(messageId) == nullptr);
    REQUIRE(store.isDismissed(messageId));

    REQUIRE_FALSE(store.put(createMessage(messageId, channelId, "Late arrived ephemeral")));
    REQUIRE(store.find(messageId) == nullptr);
    REQUIRE(store.isDismissed(messageId));
}
