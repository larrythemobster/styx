#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"
#include "Platform/Native.hpp"
#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"

namespace Styx {

namespace Storage {
class ChannelRepository;
class RoleRepository;
} // namespace Storage

namespace Core {

struct MemberListItem
{
    enum class Type {
        Group,
        Member,
        Placeholder
    };
    Type type = Type::Placeholder;

    std::string groupId; // role snowflake string, "online", or "offline"
    std::string groupName;
    int groupCount = 0;
    Platform::Color groupColor;

    Discord::Member member;
    Snowflake userId;
    std::string displayName;
    Platform::Color roleColor;
};

struct ListData
{
    std::unordered_map<int, MemberListItem> items;
    std::vector<Discord::GuildMemberListUpdate::Group> groups;
};

struct GuildListState
{
    std::unordered_map<std::string, ListData> lists; // listId -> items + groups
    std::unordered_map<std::string, Discord::Role> roleCache;
    int memberCount = 0;
    int onlineCount = 0;
};

class MemberListManager : public Platform::Trackable
{
public:
    explicit MemberListManager(Storage::ChannelRepository &channelRepo,
                               Storage::RoleRepository &roleRepo);

    void setActiveChannel(Snowflake guildId, Snowflake channelId);
    void clear();
    void clearGuild(Snowflake guildId);

    void handleMemberListUpdate(const Discord::GuildMemberListUpdate &update);

    void handleRoleCreated(Snowflake guildId, const Discord::Role &role);
    void handleRoleUpdated(Snowflake guildId, const Discord::Role &role);
    void handleRoleDeleted(Snowflake guildId, Snowflake roleId);

    void updateSubscriptionRange(int firstVisible, int lastVisible);

    [[nodiscard]] int totalItemCount() const;
    [[nodiscard]] const MemberListItem *itemAt(int index) const;
    [[nodiscard]] bool isLoaded(int index) const;
    [[nodiscard]] Snowflake currentGuildId() const { return activeGuildId; }
    [[nodiscard]] Snowflake currentChannelId() const { return activeChannelId; }
    [[nodiscard]] int totalMemberCount() const;
    [[nodiscard]] int onlineCount() const;
    [[nodiscard]] const std::vector<std::pair<int, int>> &currentRanges() const { return ranges; }
    [[nodiscard]] std::vector<int> indicesForUsers(const std::unordered_set<Snowflake> &userIds) const;

    static std::string computeListId(const std::vector<Discord::PermissionOverwrite> &overwrites, Discord::Permissions everyonePermissions);

    Platform::Signal<> listAboutToReset;
    Platform::Signal<> listReset;
    Platform::Signal<const std::vector<int> &> itemsChanged;
    Platform::Signal<Snowflake, Snowflake, const std::vector<std::pair<int, int>> &> subscriptionRequested;

private:
    GuildListState *activeGuildState();
    const GuildListState *activeGuildState() const;
    ListData *activeListData();
    const ListData *activeListData() const;

    MemberListItem syncItemToListItem(const Discord::GuildMemberListUpdate::SyncItem &syncItem,
                                      const GuildListState &guildState, const ListData &listData);
    void resolveGroupInfo(MemberListItem &item, const GuildListState &guildState,
                          const ListData &listData);
    void resolveMemberInfo(MemberListItem &item, const GuildListState &guildState);
    void processSync(const Discord::GuildMemberListUpdate::ListOp &op, ListData &listData,
                     const GuildListState &guildState);
    void processInsert(const Discord::GuildMemberListUpdate::ListOp &op, ListData &listData,
                       const GuildListState &guildState);
    void processUpdate(const Discord::GuildMemberListUpdate::ListOp &op, ListData &listData,
                       const GuildListState &guildState);
    void processDelete(const Discord::GuildMemberListUpdate::ListOp &op, ListData &listData);
    void processInvalidate(const Discord::GuildMemberListUpdate::ListOp &op, ListData &listData);

    void evictUnsubscribedItems(const std::vector<std::pair<int, int>> &oldRanges,
                                const std::vector<std::pair<int, int>> &newRanges);
    void applyAndSendRanges(const std::vector<std::pair<int, int>> &newRanges);
    void flushPendingRanges();
    static bool indexInRanges(int index, const std::vector<std::pair<int, int>> &ranges);

    Storage::ChannelRepository &channelRepo;
    Storage::RoleRepository &roleRepo;
    std::unordered_map<Snowflake, GuildListState> guildStates;
    Snowflake activeGuildId;
    Snowflake activeChannelId;
    std::string listId;
    std::vector<std::pair<int, int>> ranges;
    Platform::Timer responseTimer;
    bool awaitingResponse = false;
    bool hasPendingRanges = false;
    std::vector<std::pair<int, int>> pendingRanges;
};

} // namespace Core
} // namespace Styx
