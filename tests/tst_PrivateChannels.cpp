#include <catch2/catch_test_macros.hpp>

#include <vector>
#include <string>

#include <nlohmann/json.hpp>

#include "Core/PrivateChannels.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Enums.hpp"

using Styx::Core::Snowflake;
using namespace Styx::Discord;
using namespace Styx::Core;

namespace {

User makeUser(Snowflake id, const std::string &username,
              const std::string &globalName = {},
              const std::string &avatar = {})
{
    User u;
    u.id = id;
    u.username = username;
    if (!globalName.empty())
        u.globalName = globalName;
    if (!avatar.empty())
        u.avatar = avatar;
    return u;
}

Channel makeChannel(Snowflake id, ChannelType type = ChannelType::GROUP_DM)
{
    Channel ch;
    ch.id = id;
    ch.type = type;
    return ch;
}

} // namespace

TEST_CASE("mergePrivateChannel: omitted fields preserve baseline", "[private_channels]")
{
    const Snowflake channelId(100);
    const Snowflake ownerId(200);
    const Snowflake memberA(201);
    const Snowflake memberB(202);
    const Snowflake lastMsgId(500);

    Channel baseline = makeChannel(channelId, ChannelType::GROUP_DM);
    baseline.name = "Baseline Name";
    baseline.icon = "baseline_icon_hash";
    baseline.ownerId = ownerId;
    baseline.recipientIds = std::vector<Snowflake>{ memberA, memberB };
    baseline.lastMessageId = lastMsgId;
    baseline.topic = "Baseline Topic";

    Channel update = makeChannel(channelId, ChannelType::GROUP_DM);
    update.topic = "New Topic";

    const Channel merged = mergePrivateChannel(baseline, update);

    REQUIRE(merged.id == channelId);
    REQUIRE(merged.type == ChannelType::GROUP_DM);
    REQUIRE(merged.name.valueOr("") == "Baseline Name");
    REQUIRE(merged.icon.valueOr("") == "baseline_icon_hash");
    REQUIRE(merged.ownerId == ownerId);
    REQUIRE(merged.recipientIds.hasValue());
    REQUIRE(merged.recipientIds->size() == 2);
    REQUIRE(merged.recipientIds->at(0) == memberA);
    REQUIRE(merged.recipientIds->at(1) == memberB);
    REQUIRE(merged.lastMessageId == lastMsgId);
    REQUIRE(merged.topic.valueOr("") == "New Topic");
    REQUIRE_FALSE(merged.recipients.hasValue());
}

TEST_CASE("mergePrivateChannel: explicit null name and icon clear baseline values", "[private_channels]")
{
    const Snowflake channelId(101);

    Channel baseline = makeChannel(channelId, ChannelType::GROUP_DM);
    baseline.name = "Existing Name";
    baseline.icon = "existing_icon_hash";

    const auto updateJson = nlohmann::json::parse(R"({
        "id": "101",
        "name": null,
        "icon": null
    })");
    const Channel update = Channel::fromJson(updateJson);

    const Channel merged = mergePrivateChannel(baseline, update);

    REQUIRE(merged.name.isNull());
    REQUIRE(merged.icon.isNull());
}

TEST_CASE("mergePrivateChannel: explicit empty membership clears baseline recipients", "[private_channels]")
{
    const Snowflake channelId(102);
    const Snowflake memberA(301);
    const Snowflake memberB(302);

    Channel baseline = makeChannel(channelId, ChannelType::GROUP_DM);
    baseline.recipientIds = std::vector<Snowflake>{ memberA, memberB };

    Channel emptyRecipientsUpdate = makeChannel(channelId, ChannelType::GROUP_DM);
    emptyRecipientsUpdate.recipients = std::vector<User>{};

    const Channel mergedFromUsers = mergePrivateChannel(baseline, emptyRecipientsUpdate);
    REQUIRE(mergedFromUsers.recipientIds.hasValue());
    REQUIRE(mergedFromUsers.recipientIds->empty());
    REQUIRE_FALSE(mergedFromUsers.recipients.hasValue());

    Channel emptyIdsUpdate = makeChannel(channelId, ChannelType::GROUP_DM);
    emptyIdsUpdate.recipientIds = std::vector<Snowflake>{};

    const Channel mergedFromIds = mergePrivateChannel(baseline, emptyIdsUpdate);
    REQUIRE(mergedFromIds.recipientIds.hasValue());
    REQUIRE(mergedFromIds.recipientIds->empty());
    REQUIRE_FALSE(mergedFromIds.recipients.hasValue());
}

TEST_CASE("mergePrivateChannel: recipients override stale recipientIds and normalize/deduplicate", "[private_channels]")
{
    const Snowflake channelId(103);
    const Snowflake user1(401);
    const Snowflake user2(402);
    const Snowflake staleA(991);
    const Snowflake staleB(992);

    Channel baseline = makeChannel(channelId, ChannelType::GROUP_DM);
    baseline.recipientIds = std::vector<Snowflake>{ Snowflake(801) };

    Channel update = makeChannel(channelId, ChannelType::GROUP_DM);
    update.recipientIds = std::vector<Snowflake>{ staleA, staleB };

    std::vector<User> userList;
    userList.push_back(makeUser(user1, "alice"));
    userList.push_back(makeUser(user2, "bob"));
    userList.push_back(makeUser(user1, "alice_dup"));
    userList.push_back(makeUser(Snowflake(0), "zero_id"));
    userList.push_back(makeUser(Snowflake::Invalid, "invalid_id"));
    update.recipients = userList;

    const Channel merged = mergePrivateChannel(baseline, update);

    REQUIRE(merged.recipientIds.hasValue());
    REQUIRE(merged.recipientIds->size() == 2);
    REQUIRE(merged.recipientIds->at(0) == user1);
    REQUIRE(merged.recipientIds->at(1) == user2);
    REQUIRE_FALSE(merged.recipients.hasValue());
}

TEST_CASE("mergePrivateChannel: last message id never regresses to an older id", "[private_channels]")
{
    const Snowflake channelId(104);
    const Snowflake initialMsg(500);
    const Snowflake olderMsg(400);
    const Snowflake newerMsg(600);

    Channel baseline = makeChannel(channelId, ChannelType::GROUP_DM);
    baseline.lastMessageId = initialMsg;

    Channel olderUpdate = makeChannel(channelId, ChannelType::GROUP_DM);
    olderUpdate.lastMessageId = olderMsg;

    const Channel mergedOlder = mergePrivateChannel(baseline, olderUpdate);
    REQUIRE(mergedOlder.lastMessageId == initialMsg);

    Channel newerUpdate = makeChannel(channelId, ChannelType::GROUP_DM);
    newerUpdate.lastMessageId = newerMsg;

    const Channel mergedNewer = mergePrivateChannel(baseline, newerUpdate);
    REQUIRE(mergedNewer.lastMessageId == newerMsg);

    Channel undefinedUpdate = makeChannel(channelId, ChannelType::GROUP_DM);
    const Channel mergedUndefined = mergePrivateChannel(baseline, undefinedUpdate);
    REQUIRE(mergedUndefined.lastMessageId == initialMsg);
}

TEST_CASE("applyPrivateRecipient: duplicate add and remove are idempotent and preserve neighbor order", "[private_channels]")
{
    const Snowflake u1(601);
    const Snowflake u2(602);
    const Snowflake u3(603);
    const Snowflake u4(604);

    Channel channel = makeChannel(Snowflake(105), ChannelType::GROUP_DM);
    channel.recipientIds = std::vector<Snowflake>{ u1, u2, u3 };

    REQUIRE_FALSE(applyPrivateRecipient(channel, u2, true));
    REQUIRE(channel.recipientIds->size() == 3);
    REQUIRE(channel.recipientIds->at(0) == u1);
    REQUIRE(channel.recipientIds->at(1) == u2);
    REQUIRE(channel.recipientIds->at(2) == u3);

    REQUIRE(applyPrivateRecipient(channel, u4, true));
    REQUIRE(channel.recipientIds->size() == 4);
    REQUIRE(channel.recipientIds->at(0) == u1);
    REQUIRE(channel.recipientIds->at(1) == u2);
    REQUIRE(channel.recipientIds->at(2) == u3);
    REQUIRE(channel.recipientIds->at(3) == u4);

    REQUIRE_FALSE(applyPrivateRecipient(channel, u4, true));
    REQUIRE(channel.recipientIds->size() == 4);

    REQUIRE(applyPrivateRecipient(channel, u2, false));
    REQUIRE(channel.recipientIds->size() == 3);
    REQUIRE(channel.recipientIds->at(0) == u1);
    REQUIRE(channel.recipientIds->at(1) == u3);
    REQUIRE(channel.recipientIds->at(2) == u4);

    REQUIRE_FALSE(applyPrivateRecipient(channel, u2, false));
    REQUIRE(channel.recipientIds->size() == 3);

    REQUIRE_FALSE(applyPrivateRecipient(channel, Snowflake(999), false));
    REQUIRE(channel.recipientIds->size() == 3);

    REQUIRE_FALSE(applyPrivateRecipient(channel, Snowflake(0), true));
    REQUIRE_FALSE(applyPrivateRecipient(channel, Snowflake::Invalid, true));
}

TEST_CASE("mergePrivateUser: preserves identity from partial payloads and updates supplied fields", "[private_channels]")
{
    const Snowflake userId(701);

    User baseline = makeUser(userId, "alice", "Alice Wonderland", "avatar_hash_1");
    baseline.banner = "banner_hash_1";
    baseline.bot = false;

    const auto idOnlyJson = nlohmann::json::parse(R"({
        "id": "701"
    })");
    const User idOnlyUpdate = User::fromJson(idOnlyJson);

    const User mergedIdOnly = mergePrivateUser(baseline, idOnlyUpdate);
    REQUIRE(mergedIdOnly.id == userId);
    REQUIRE(mergedIdOnly.username.get() == "alice");
    REQUIRE(mergedIdOnly.globalName.valueOr("") == "Alice Wonderland");
    REQUIRE(mergedIdOnly.avatar.valueOr("") == "avatar_hash_1");
    REQUIRE(mergedIdOnly.banner.valueOr("") == "banner_hash_1");
    REQUIRE_FALSE(mergedIdOnly.bot.valueOr(true));

    const auto nameUpdateJson = nlohmann::json::parse(R"({
        "id": "701",
        "username": "alice_renamed"
    })");
    const User nameUpdate = User::fromJson(nameUpdateJson);

    const User mergedName = mergePrivateUser(baseline, nameUpdate);
    REQUIRE(mergedName.username.get() == "alice_renamed");
    REQUIRE(mergedName.globalName.valueOr("") == "Alice Wonderland");
    REQUIRE(mergedName.avatar.valueOr("") == "avatar_hash_1");

    const auto nullGlobalJson = nlohmann::json::parse(R"({
        "id": "701",
        "global_name": null
    })");
    const User nullGlobalUpdate = User::fromJson(nullGlobalJson);

    const User mergedNullGlobal = mergePrivateUser(baseline, nullGlobalUpdate);
    REQUIRE(mergedNullGlobal.username.get() == "alice");
    REQUIRE(mergedNullGlobal.globalName.isNull());
    REQUIRE(mergedNullGlobal.avatar.valueOr("") == "avatar_hash_1");

    const auto nullAvatarJson = nlohmann::json::parse(R"({
        "id": "701",
        "avatar": null
    })");
    const User nullAvatarUpdate = User::fromJson(nullAvatarJson);

    const User mergedNullAvatar = mergePrivateUser(baseline, nullAvatarUpdate);
    REQUIRE(mergedNullAvatar.username.get() == "alice");
    REQUIRE(mergedNullAvatar.globalName.valueOr("") == "Alice Wonderland");
    REQUIRE(mergedNullAvatar.avatar.isNull());

    User progUpdate;
    progUpdate.id = userId;
    progUpdate.username = "alice_prog";
    progUpdate.globalName = "Alice Programmatic";
    progUpdate.avatar = "avatar_prog";

    const User mergedProg = mergePrivateUser(baseline, progUpdate);
    REQUIRE(mergedProg.username.get() == "alice_prog");
    REQUIRE(mergedProg.globalName.valueOr("") == "Alice Programmatic");
    REQUIRE(mergedProg.avatar.valueOr("") == "avatar_prog");
}
