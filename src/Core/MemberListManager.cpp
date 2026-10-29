#include "MemberListManager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "MurmurHash3.hpp"
#include "Storage/ChannelRepository.hpp"
#include "Storage/RoleRepository.hpp"

static uint32_t mmh32(const std::string &input, uint32_t seed)
{
    uint32_t result;
    MurmurHash3_x86_32(input.data(), static_cast<int>(input.size()), seed, &result);
    return result;
}

namespace Styx {
namespace Core {

MemberListManager::MemberListManager(Storage::ChannelRepository &channelRepo,
                                     Storage::RoleRepository &roleRepo)
    : channelRepo(channelRepo), roleRepo(roleRepo)
{
}

GuildListState *MemberListManager::activeGuildState()
{
    if (!activeGuildId.isValid())
        return nullptr;
    auto it = guildStates.find(activeGuildId);
    return it != guildStates.end() ? &it->second : nullptr;
}

const GuildListState *MemberListManager::activeGuildState() const
{
    if (!activeGuildId.isValid())
        return nullptr;
    auto it = guildStates.find(activeGuildId);
    return it != guildStates.end() ? &it->second : nullptr;
}

ListData *MemberListManager::activeListData()
{
    auto *gs = activeGuildState();
    if (!gs || listId.empty())
        return nullptr;
    auto it = gs->lists.find(listId);
    return it != gs->lists.end() ? &it->second : nullptr;
}

const ListData *MemberListManager::activeListData() const
{
    const auto *gs = activeGuildState();
    if (!gs || listId.empty())
        return nullptr;
    auto it = gs->lists.find(listId);
    return it != gs->lists.end() ? &it->second : nullptr;
}

void MemberListManager::setActiveChannel(Snowflake guildId, Snowflake channelId)
{
    if (activeGuildId == guildId && activeChannelId == channelId)
        return;

    bool sameGuild = activeGuildId == guildId;
    responseTimer.stop();
    awaitingResponse = false;
    hasPendingRanges = false;
    activeGuildId = guildId;
    activeChannelId = channelId;

    GuildListState &gs = guildStates[activeGuildId];
    if (gs.roleCache.empty()) {
        const auto roles = roleRepo.getRolesForGuild(activeGuildId);
        for (const auto &role : roles)
            gs.roleCache.insert_or_assign(role.id.get().toString(), role);
    }

    auto overwrites = channelRepo.getPermissionOverwrites(activeChannelId);
    Discord::Permissions everyonePerms;
    auto rit = gs.roleCache.find(activeGuildId.toString());
    if (rit != gs.roleCache.end())
        everyonePerms = rit->second.permissions.get();
    std::string newListId = computeListId(overwrites, everyonePerms);
    bool listChanged = newListId != listId || !sameGuild;
    listId = newListId;
    gs.lists[newListId];

    ranges = {{0, 99}};
    if (listChanged) {
        listAboutToReset.publish();
        listReset.publish();
    }

    awaitingResponse = true;
    responseTimer.start(std::chrono::milliseconds(5000), false, [this] { flushPendingRanges(); });
    subscriptionRequested.publish(activeGuildId, activeChannelId, ranges);
}

void MemberListManager::clear()
{
    responseTimer.stop();
    awaitingResponse = false;
    hasPendingRanges = false;
    listAboutToReset.publish();
    activeGuildId = Snowflake::Invalid;
    activeChannelId = Snowflake::Invalid;
    listId.clear();
    ranges.clear();
    listReset.publish();
}

void MemberListManager::clearGuild(Snowflake guildId)
{
    guildStates.erase(guildId);
    if (activeGuildId == guildId)
        clear();
}

void MemberListManager::handleRoleCreated(Snowflake guildId, const Discord::Role &role)
{
    auto it = guildStates.find(guildId);
    if (it == guildStates.end())
        return;
    it->second.roleCache.insert_or_assign(role.id.get().toString(), role);
}

void MemberListManager::handleRoleUpdated(Snowflake guildId, const Discord::Role &role)
{
    auto it = guildStates.find(guildId);
    if (it == guildStates.end())
        return;

    GuildListState &gs = it->second;
    std::string roleIdStr = role.id.get().toString();
    gs.roleCache.insert_or_assign(roleIdStr, role);
    bool changed = false;
    for (auto &[id, ld] : gs.lists) {
        for (auto &[index, item] : ld.items) {
            if (item.type == MemberListItem::Type::Group && item.groupId == roleIdStr) {
                resolveGroupInfo(item, gs, ld);
                changed = true;
            } else if (item.type == MemberListItem::Type::Member && !item.member.roles.isUndefined()) {
                for (const auto &rid : item.member.roles.get()) {
                    if (rid.toString() == roleIdStr) {
                        resolveMemberInfo(item, gs);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }
    if (changed && guildId == activeGuildId) {
        listAboutToReset.publish();
        listReset.publish();
    }
}

void MemberListManager::handleRoleDeleted(Snowflake guildId, Snowflake roleId)
{
    auto it = guildStates.find(guildId);
    if (it == guildStates.end())
        return;

    GuildListState &gs = it->second;
    std::string roleIdStr = roleId.toString();
    gs.roleCache.erase(roleIdStr);
    bool changed = false;
    for (auto &[id, ld] : gs.lists) {
        for (auto &[index, item] : ld.items) {
            if (item.type == MemberListItem::Type::Member && !item.member.roles.isUndefined()) {
                for (const auto &rid : item.member.roles.get()) {
                    if (rid.toString() == roleIdStr) {
                        resolveMemberInfo(item, gs);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }
    if (changed && guildId == activeGuildId) {
        listAboutToReset.publish();
        listReset.publish();
    }
}

static bool sameGroups(const std::vector<Discord::GuildMemberListUpdate::Group> &a,
                       const std::vector<Discord::GuildMemberListUpdate::Group> &b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id.get() != b[i].id.get() || a[i].count.get() != b[i].count.get())
            return false;
    }
    return true;
}

static std::optional<std::vector<int>> inPlaceUpdateIndices(const Discord::GuildMemberListUpdate &update, const ListData &current)
{
    if (!update.groups->empty() && !sameGroups(update.groups.get(), current.groups))
        return std::nullopt;
    std::vector<int> indices;
    for (const auto &op : update.ops.get()) {
        if (op.op.get() != "UPDATE" || op.index.isUndefined() || op.item.isUndefined())
            return std::nullopt;
        indices.push_back(op.index.get());
    }
    return indices;
}

void MemberListManager::handleMemberListUpdate(const Discord::GuildMemberListUpdate &update)
{
    GuildListState &gs = guildStates[update.guildId.get()];
    gs.memberCount = update.memberCount.get();
    gs.onlineCount = update.onlineCount.get();
    ListData &ld = gs.lists[update.id.get()];
    const bool isActiveList = update.guildId.get() == activeGuildId && update.id.get() == listId;
    const auto updatedIndices = isActiveList ? inPlaceUpdateIndices(update, ld) : std::nullopt;
    const bool inPlace = updatedIndices.has_value();
    if (!update.groups->empty())
        ld.groups = update.groups.get();
    if (isActiveList) {
        awaitingResponse = false;
        responseTimer.stop();
    }
    if (isActiveList && !inPlace)
        listAboutToReset.publish();

    for (const auto &op : update.ops.get()) {
        const auto &opType = op.op.get();
        if (opType == "SYNC")
            processSync(op, ld, gs);
        else if (opType == "INSERT")
            processInsert(op, ld, gs);
        else if (opType == "UPDATE")
            processUpdate(op, ld, gs);
        else if (opType == "DELETE")
            processDelete(op, ld);
        else if (opType == "INVALIDATE")
            processInvalidate(op, ld);
    }
    if (isActiveList) {
        if (inPlace)
            itemsChanged.publish(updatedIndices.value());
        else
            listReset.publish();
    }
    if (isActiveList && hasPendingRanges && pendingRanges != ranges)
        applyAndSendRanges(pendingRanges);
    else if (isActiveList)
        hasPendingRanges = false;
}

int MemberListManager::totalItemCount() const
{
    const auto *ld = activeListData();
    if (!ld)
        return 0;
    int count = 0;
    for (const auto &group : ld->groups)
        count += 1 + group.count.get();
    return count;
}

const MemberListItem *MemberListManager::itemAt(int index) const
{
    const auto *ld = activeListData();
    if (!ld)
        return nullptr;
    auto it = ld->items.find(index);
    return it != ld->items.end() ? &it->second : nullptr;
}

std::vector<int> MemberListManager::indicesForUsers(const std::unordered_set<Snowflake> &userIds) const
{
    std::vector<int> indices;
    const auto *ld = activeListData();
    if (!ld || userIds.empty())
        return indices;
    for (const auto &[index, item] : ld->items) {
        if (item.type == MemberListItem::Type::Member && userIds.contains(item.userId))
            indices.push_back(index);
    }
    return indices;
}

bool MemberListManager::isLoaded(int index) const
{
    const auto *ld = activeListData();
    return ld && ld->items.contains(index);
}

int MemberListManager::totalMemberCount() const
{
    const auto *gs = activeGuildState();
    return gs ? gs->memberCount : 0;
}

int MemberListManager::onlineCount() const
{
    const auto *gs = activeGuildState();
    return gs ? gs->onlineCount : 0;
}

void MemberListManager::updateSubscriptionRange(int firstVisible, int lastVisible)
{
    if (!activeGuildId.isValid() || !activeChannelId.isValid())
        return;
    std::vector<std::pair<int, int>> newRanges = {{0, 99}};
    if (lastVisible > 99) {
        int firstRangeStart = (firstVisible / 100) * 100;
        if (firstRangeStart > 0)
            newRanges.push_back({firstRangeStart, firstRangeStart + 99});
        int lastRangeStart = (lastVisible / 100) * 100;
        if (lastRangeStart != firstRangeStart && lastRangeStart > 0) {
            if (newRanges.size() < 3)
                newRanges.push_back({lastRangeStart, lastRangeStart + 99});
        }
    }
    if (newRanges == ranges && !hasPendingRanges)
        return;
    if (awaitingResponse) {
        pendingRanges = newRanges;
        hasPendingRanges = true;
        return;
    }
    if (newRanges == ranges)
        return;
    applyAndSendRanges(newRanges);
}

void MemberListManager::applyAndSendRanges(const std::vector<std::pair<int, int>> &newRanges)
{
    auto oldRanges = ranges;
    ranges = newRanges;
    hasPendingRanges = false;
    evictUnsubscribedItems(oldRanges, newRanges);
    awaitingResponse = true;
    responseTimer.start(std::chrono::milliseconds(5000), false, [this] { flushPendingRanges(); });
    subscriptionRequested.publish(activeGuildId, activeChannelId, ranges);
}

void MemberListManager::flushPendingRanges()
{
    awaitingResponse = false;
    if (hasPendingRanges && pendingRanges != ranges)
        applyAndSendRanges(pendingRanges);
    else
        hasPendingRanges = false;
}

void MemberListManager::evictUnsubscribedItems(const std::vector<std::pair<int, int>> &,
                                               const std::vector<std::pair<int, int>> &newRanges)
{
    auto *ld = activeListData();
    if (!ld)
        return;
    std::vector<int> toRemove;
    for (const auto &[index, item] : ld->items) {
        if (!indexInRanges(index, newRanges))
            toRemove.push_back(index);
    }
    if (toRemove.empty())
        return;
    listAboutToReset.publish();
    for (int idx : toRemove)
        ld->items.erase(idx);
    listReset.publish();
}

bool MemberListManager::indexInRanges(int index, const std::vector<std::pair<int, int>> &ranges)
{
    for (const auto &r : ranges) {
        if (index >= r.first && index <= r.second)
            return true;
    }
    return false;
}

MemberListItem MemberListManager::syncItemToListItem(
        const Discord::GuildMemberListUpdate::SyncItem &syncItem,
        const GuildListState &guildState, const ListData &listData)
{
    MemberListItem listItem;
    if (!syncItem.group.isUndefined() && syncItem.group->id.hasValue()) {
        listItem.type = MemberListItem::Type::Group;
        listItem.groupId = syncItem.group->id.get();
        listItem.groupCount = syncItem.group->count.get();
        resolveGroupInfo(listItem, guildState, listData);
    } else if (!syncItem.member.isUndefined()) {
        listItem.type = MemberListItem::Type::Member;
        listItem.member = syncItem.member.get();
        listItem.member.presence.undefine();
        if (!syncItem.member->user.isUndefined())
            listItem.userId = syncItem.member->user->id.get();
        resolveMemberInfo(listItem, guildState);
    }
    return listItem;
}

void MemberListManager::resolveGroupInfo(MemberListItem &item, const GuildListState &guildState, const ListData &listData)
{
    for (const auto &group : listData.groups) {
        if (group.id.get() == item.groupId) {
            item.groupCount = group.count.get();
            break;
        }
    }
    if (item.groupId == "online") {
        item.groupName = "Online";
        item.groupColor = {};
    } else if (item.groupId == "offline") {
        item.groupName = "Offline";
        item.groupColor = {};
    } else {
        auto it = guildState.roleCache.find(item.groupId);
        if (it != guildState.roleCache.end()) {
            item.groupName = it->second.name.get();
            if (it->second.hasColor())
                item.groupColor = it->second.getColor();
        } else {
            item.groupName = "Unknown Role";
        }
    }
}

void MemberListManager::resolveMemberInfo(MemberListItem &item, const GuildListState &guildState)
{
    const auto &member = item.member;
    if (!member.nick.isUndefined() && !member.nick.isNull() && !member.nick->empty())
        item.displayName = member.nick.get();
    else if (!member.user.isUndefined())
        item.displayName = member.user->getDisplayName();
    if (!member.roles.isUndefined()) {
        int highestPos = -1;
        int bestColor = 0;
        for (const auto &roleId : member.roles.get()) {
            auto it = guildState.roleCache.find(roleId.toString());
            if (it != guildState.roleCache.end()) {
                const auto &role = it->second;
                if (!role.color.isUndefined() && role.color.get() != 0 && role.position.get() > highestPos) {
                    highestPos = role.position.get();
                    bestColor = role.color.get();
                }
            }
        }
        if (bestColor != 0)
            item.roleColor = Platform::Color::rgb(static_cast<uint32_t>(bestColor));
    }
}

void MemberListManager::processSync(const Discord::GuildMemberListUpdate::ListOp &op,
                                    ListData &listData, const GuildListState &guildState)
{
    if (op.range.isUndefined() || op.items.isUndefined() || op.items->empty())
        return;
    int idx = op.range->first;
    for (const auto &syncItem : op.items.get()) {
        listData.items[idx] = syncItemToListItem(syncItem, guildState, listData);
        ++idx;
    }
}

void MemberListManager::processInsert(const Discord::GuildMemberListUpdate::ListOp &op,
                                      ListData &listData, const GuildListState &guildState)
{
    if (op.index.isUndefined() || op.item.isUndefined())
        return;
    int insertIdx = op.index.get();
    std::vector<int> keys;
    keys.reserve(listData.items.size());
    for (const auto &[key, item] : listData.items) {
        if (key >= insertIdx)
            keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), std::greater<int>());
    for (int key : keys) {
        auto node = listData.items.extract(key);
        node.key() = key + 1;
        listData.items.insert(std::move(node));
    }
    listData.items[insertIdx] = syncItemToListItem(op.item.get(), guildState, listData);
}

void MemberListManager::processUpdate(const Discord::GuildMemberListUpdate::ListOp &op,
                                      ListData &listData, const GuildListState &guildState)
{
    if (op.index.isUndefined() || op.item.isUndefined())
        return;
    listData.items[op.index.get()] = syncItemToListItem(op.item.get(), guildState, listData);
}

void MemberListManager::processDelete(const Discord::GuildMemberListUpdate::ListOp &op,
                                      ListData &listData)
{
    if (op.index.isUndefined())
        return;
    int deleteIdx = op.index.get();
    listData.items.erase(deleteIdx);
    std::vector<int> keys;
    keys.reserve(listData.items.size());
    for (const auto &[key, item] : listData.items) {
        if (key > deleteIdx)
            keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    for (int key : keys) {
        auto node = listData.items.extract(key);
        node.key() = key - 1;
        listData.items.insert(std::move(node));
    }
}

void MemberListManager::processInvalidate(const Discord::GuildMemberListUpdate::ListOp &op,
                                          ListData &listData)
{
    if (op.range.isUndefined())
        return;
    int rangeStart = op.range->first;
    int rangeEnd = op.range->second;
    for (int i = rangeStart; i <= rangeEnd; ++i)
        listData.items.erase(i);
}

std::string MemberListManager::computeListId(const std::vector<Discord::PermissionOverwrite> &overwrites, Discord::Permissions everyonePermissions)
{
    bool hasDenyRules = false;
    for (const auto &ow : overwrites) {
        if (ow.deny->testFlag(Discord::Permission::VIEW_CHANNEL)) {
            hasDenyRules = true;
            break;
        }
    }
    if (!hasDenyRules && everyonePermissions.testFlag(Discord::Permission::VIEW_CHANNEL))
        return "everyone";
    std::vector<std::string> hashInputs;
    for (const auto &ow : overwrites) {
        if (ow.allow->testFlag(Discord::Permission::VIEW_CHANNEL))
            hashInputs.push_back("allow:" + ow.id.get().toString());
        else if (ow.deny->testFlag(Discord::Permission::VIEW_CHANNEL))
            hashInputs.push_back("deny:" + ow.id.get().toString());
    }
    std::sort(hashInputs.begin(), hashInputs.end());
    std::string input;
    for (const auto &part : hashInputs) {
        if (!input.empty())
            input += ',';
        input += part;
    }
    return std::to_string(mmh32(input, 0));
}

} // namespace Core
} // namespace Styx
