#include "PresenceManager.hpp"

#include <algorithm>
#include <chrono>

#include "Platform/Native.hpp"

#include "ActivityFormat.hpp"
#include "Core/Logging.hpp"

namespace Styx {
namespace Core {

namespace {

bool sameEntry(const PresenceEntry &a, const PresenceEntry &b)
{
    return a.status == b.status &&
           a.clientStatus.isMobileOnly() == b.clientStatus.isMobileOnly() &&
           a.sortedActivities == b.sortedActivities;
}

bool allOffline(const std::unordered_map<Snowflake, PresenceEntry> &guildSlots)
{
    for (const auto &[guildId, entry] : guildSlots) {
        if (entry.status != Discord::StatusType::OFFLINE)
            return false;
    }

    return true;
}

} // namespace


void PresenceManager::setSelfUserId(Snowflake userId)
{
    selfUserId = userId;
}

void PresenceManager::reset()
{
    for (const auto &[userId, slots] : presences)
        markDirty(userId);

    presences.clear();
    selfPresence = PresenceEntry();
    selfCustomStatus.reset();
    sessionsReplaceSeen = false;
    showCurrentGame = true;
    markDirty(selfUserId);
}

void PresenceManager::markDirty(Snowflake userId)
{
    if (!userId.isValid())
        return;

    dirtyUsers.insert(userId);
    if (!flushTimer.active())
        flushTimer.start(std::chrono::milliseconds(0), false, [this] { flushDirty(); });
}

void PresenceManager::flushDirty()
{
    if (dirtyUsers.empty())
        return;

    const std::vector<Snowflake> userIds(dirtyUsers.cbegin(), dirtyUsers.cend());
    dirtyUsers.clear();
    presencesChanged.publish(userIds);
}

bool PresenceManager::apply(const Discord::Presence &presence, Snowflake guildId)
{
    const Snowflake userId = presence.userId();
    if (!userId.isValid())
        return false;

    if (userId == selfUserId)
        return false;

    PresenceEntry entry;
    entry.status = presence.statusType();
    if (presence.clientStatus.hasValue())
        entry.clientStatus = presence.clientStatus.get();
    if (presence.activities.hasValue())
        entry.sortedActivities = ActivityFormat::sortAndFilter(presence.activities.get());
    entry.receivedAtMs = Platform::DateTime::now().unixMillis();

    const bool offline = entry.status == Discord::StatusType::OFFLINE;
    if (offline) {
        if (!presences.contains(userId))
            return false;
        entry.sortedActivities.clear();
    }

    auto &guildSlots = presences[userId];
    auto existing = guildSlots.find(guildId);
    if (existing != guildSlots.end() && sameEntry(existing->second, entry)) {
        existing->second.receivedAtMs = entry.receivedAtMs;
        return false;
    }

    guildSlots.insert_or_assign(guildId, entry);
    if (offline && allOffline(guildSlots))
        presences.erase(userId);

    markDirty(userId);
    return true;
}

void PresenceManager::ingestMember(const Discord::Member &member, Snowflake guildId)
{
    if (!member.presence.hasValue())
        return;

    Discord::Presence presence = member.presence.get();
    if (!presence.userId().isValid()) {
        if (member.user.hasValue()) {
            presence.user = member.user.get();
        } else if (member.userId.hasValue()) {
            Discord::User user;
            user.id = member.userId.get();
            presence.user = user;
        }
    }

    apply(presence, guildId);
}

void PresenceManager::loadMergedPresences(const Discord::MergedPresences &merged, const std::vector<Snowflake> &guildIdsInOrder)
{
    int count = 0;

    if (merged.friends.hasValue()) {
        for (const Discord::Presence &presence : merged.friends.get()) {
            if (apply(presence, {}))
                count++;
        }
    }

    if (merged.guilds.hasValue()) {
        const auto &guilds = merged.guilds.get();
        const auto guildCount = std::min(guilds.size(), guildIdsInOrder.size());
        for (std::size_t i = 0; i < guildCount; i++) {
            for (const Discord::Presence &presence : guilds.at(i)) {
                if (apply(presence, guildIdsInOrder.at(i)))
                    count++;
            }
        }
    }

    Core::log(Core::LogLevel::Info, LogCore) << "Applied " << count << " presences";
}

void PresenceManager::rebuildSelfActivities(std::vector<Discord::Activity> sessionActivities)
{
    std::erase_if(sessionActivities, [](const Discord::Activity &activity) { return activity.isCustom(); });

    if (selfCustomStatus.has_value())
        sessionActivities.push_back(selfCustomStatus.value());

    selfPresence.sortedActivities = ActivityFormat::sortAndFilter(sessionActivities);
}

void PresenceManager::applyProtoStatus(const Proto::PreloadedUserSettings &settings)
{
    const Proto::StatusSettings status = settings.status.has_value() ? settings.status.value() : Proto::StatusSettings();
    const std::int64_t now = Platform::DateTime::now().unixMillis();

    showCurrentGame = status.showCurrentGame.value_or(true);

    if (!sessionsReplaceSeen) {
        Discord::StatusType resolved = Discord::StatusType::ONLINE;
        if (status.status.has_value()) {
            resolved = Discord::parseStatus(status.status.value());
            if (resolved == Discord::StatusType::UNKNOWN)
                resolved = Discord::StatusType::ONLINE;
        }

        if (status.statusExpiresAtMs != 0 && now > static_cast<std::int64_t>(status.statusExpiresAtMs))
            resolved = Discord::StatusType::ONLINE;

        selfPresence.status = resolved;
    }

    std::optional<Discord::Activity> custom;
    if (status.customStatus.has_value()) {
        const Proto::CustomStatus &source = status.customStatus.value();
        const bool expired = source.expiresAtMs != 0 && now > static_cast<std::int64_t>(source.expiresAtMs);
        const bool hasEmojiId = source.emojiId != 0;

        if (!expired &&
            (!source.text.empty() || !source.emojiName.empty() || hasEmojiId)) {
            Discord::Activity activity;
            activity.name = "Custom Status";
            activity.type = Discord::ActivityType::CUSTOM;
            activity.state = source.text;
            activity.createdAt = static_cast<std::int64_t>(source.createdAtMs);

            if (!source.emojiName.empty() || hasEmojiId) {
                Discord::ActivityEmoji emoji;
                if (!source.emojiName.empty())
                    emoji.name = source.emojiName;
                if (hasEmojiId)
                    emoji.id = Core::Snowflake(source.emojiId);
                activity.emoji = emoji;
            }

            custom = activity;
        }
    }

    selfCustomStatus = custom;
    rebuildSelfActivities(selfPresence.sortedActivities);
    markDirty(selfUserId);
}

const PresenceEntry *PresenceManager::newest(Snowflake userId) const
{
    auto it = presences.find(userId);
    if (it == presences.end())
        return nullptr;

    const PresenceEntry *best = nullptr;
    for (const auto &[guildId, entry] : it->second) {
        if (best == nullptr) {
            best = &entry;
            continue;
        }

        if (entry.receivedAtMs > best->receivedAtMs ||
            (entry.receivedAtMs == best->receivedAtMs &&
             entry.sortedActivities.size() > best->sortedActivities.size()))
            best = &entry;
    }

    return best;
}

const PresenceEntry *PresenceManager::resolve(Snowflake userId, Snowflake guildId) const
{
    if (userId.isValid() && userId == selfUserId)
        return &selfPresence;

    const PresenceEntry *entry = nullptr;
    if (guildId.isValid()) {
        auto it = presences.find(userId);
        if (it != presences.end()) {
            auto slot = it->second.find(guildId);
            if (slot != it->second.end())
                entry = &slot->second;
        }
    }

    if (entry == nullptr)
        entry = newest(userId);

    if (entry != nullptr && entry->status == Discord::StatusType::OFFLINE)
        return nullptr;

    return entry;
}

bool PresenceManager::hidesGames(Snowflake userId) const
{
    return userId == selfUserId && !showCurrentGame;
}

PresenceBadge PresenceManager::badge(Snowflake userId, Snowflake guildId) const
{
    const PresenceEntry *entry = resolve(userId, guildId);
    if (entry == nullptr)
        return {};

    PresenceBadge badge;
    badge.status = entry->status;
    badge.mobile = entry->clientStatus.isMobileOnly();

    if (hidesGames(userId))
        return badge;

    for (const Discord::Activity &activity : entry->sortedActivities) {
        if (activity.kind() == Discord::ActivityType::STREAMING) {
            badge.streaming = true;
            break;
        }
    }

    return badge;
}

std::vector<Discord::Activity> PresenceManager::activities(Snowflake userId, Snowflake guildId) const
{
    const PresenceEntry *entry = resolve(userId, guildId);
    if (entry == nullptr)
        return {};

    if (!hidesGames(userId))
        return entry->sortedActivities;

    std::vector<Discord::Activity> visible;
    for (const Discord::Activity &activity : entry->sortedActivities) {
        if (activity.isCustom())
            visible.push_back(activity);
    }

    return visible;
}

const Discord::Activity *PresenceManager::primaryActivity(Snowflake userId,
                                                          Snowflake guildId) const
{
    const PresenceEntry *entry = resolve(userId, guildId);
    if (entry == nullptr)
        return nullptr;

    return hidesGames(userId) ? ActivityFormat::custom(entry->sortedActivities)
                              : ActivityFormat::primary(entry->sortedActivities);
}

void PresenceManager::onPresenceUpdate(const Discord::Presence &presence)
{
    const Snowflake guildId = presence.guildId.hasValue() ? presence.guildId.get() : Snowflake();
    apply(presence, guildId);
}

void PresenceManager::onPresencesReplace(const std::vector<Discord::Presence> &replacements)
{
    if (replacements.empty())
        return;

    dropSlot({});

    for (const Discord::Presence &presence : replacements)
        apply(presence, {});
}

void PresenceManager::onSessionsReplace(const std::vector<Discord::UserSession> &sessions)
{
    if (sessions.empty())
        return;

    const Discord::UserSession *chosen = nullptr;
    for (const Discord::UserSession &session : sessions) {
        if (session.isAggregate()) {
            chosen = &session;
            break;
        }
    }

    if (chosen == nullptr) {
        for (const Discord::UserSession &session : sessions) {
            if (session.active.hasValue() && session.active.get()) {
                chosen = &session;
                break;
            }
        }
    }

    if (chosen == nullptr)
        chosen = &sessions.front();

    selfPresence.status = chosen->statusType();
    if (selfPresence.status == Discord::StatusType::UNKNOWN)
        selfPresence.status = Discord::StatusType::ONLINE;
    rebuildSelfActivities(chosen->activities.hasValue() ? chosen->activities.get()
                                                        : std::vector<Discord::Activity>());

    selfPresence.clientStatus = Discord::ClientStatus();
    selfPresence.receivedAtMs = Platform::DateTime::now().unixMillis();
    sessionsReplaceSeen = true;

    markDirty(selfUserId);
}

void PresenceManager::onGuildCreated(const Discord::GatewayGuild &guild)
{
    if (!guild.presences.hasValue())
        return;

    const Snowflake guildId = guild.properties->id;
    for (const Discord::Presence &presence : guild.presences.get())
        apply(presence, guildId);
}

void PresenceManager::onGuildRemoved(Snowflake guildId)
{
    dropSlot(guildId);
}

void PresenceManager::dropSlot(Snowflake guildId)
{
    for (auto it = presences.begin(); it != presences.end();) {
        if (!it->second.erase(guildId)) {
            ++it;
            continue;
        }

        markDirty(it->first);
        if (it->second.empty())
            it = presences.erase(it);
        else
            ++it;
    }
}

void PresenceManager::onGuildMemberListUpdate(const Discord::GuildMemberListUpdate &update)
{
    if (!update.ops.hasValue())
        return;

    const Snowflake guildId = update.guildId;

    for (const auto &op : update.ops.get()) {
        if (op.items.hasValue()) {
            for (const auto &item : op.items.get()) {
                if (item.member.hasValue())
                    ingestMember(item.member.get(), guildId);
            }
        }

        if (op.item.hasValue() && op.item->member.hasValue())
            ingestMember(op.item->member.get(), guildId);
    }
}

void PresenceManager::onGuildMembersChunk(const Discord::GuildMembersChunk &chunk)
{
    if (!chunk.presences.hasValue())
        return;

    for (const Discord::Presence &presence : chunk.presences.get())
        apply(presence, chunk.guildId);
}

} // namespace Core
} // namespace Styx
