#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Platform/Trackable.hpp"
#include "Platform/Timer.hpp"

#include "PresenceBadge.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"
#include "Core/Snowflake.hpp"
#include "Proto/UserSettings.hpp"

namespace Styx {
namespace Core {

struct PresenceEntry
{
    Discord::StatusType status = Discord::StatusType::OFFLINE;
    Discord::ClientStatus clientStatus;
    std::vector<Discord::Activity> sortedActivities;
    std::int64_t receivedAtMs = 0;
};

class PresenceManager : public Platform::Trackable
{
public:
    PresenceManager() = default;

    void setSelfUserId(Snowflake userId);
    void reset();

    void loadMergedPresences(const Discord::MergedPresences &merged,
                             const std::vector<Snowflake> &guildIdsInOrder);
    void applyProtoStatus(const Proto::PreloadedUserSettings &settings);
    [[nodiscard]] Discord::StatusType selfStatus() const { return selfPresence.status; }

    [[nodiscard]] PresenceBadge badge(Snowflake userId, Snowflake guildId = {}) const;
    [[nodiscard]] std::vector<Discord::Activity> activities(Snowflake userId, Snowflake guildId = {}) const;
    [[nodiscard]] const Discord::Activity *primaryActivity(Snowflake userId, Snowflake guildId = {}) const;

    void onPresenceUpdate(const Discord::Presence &presence);
    void onPresencesReplace(const std::vector<Discord::Presence> &presences);
    void onSessionsReplace(const std::vector<Discord::UserSession> &sessions);
    void onGuildCreated(const Discord::GatewayGuild &guild);
    void onGuildRemoved(Snowflake guildId);
    void onGuildMemberListUpdate(const Discord::GuildMemberListUpdate &update);
    void onGuildMembersChunk(const Discord::GuildMembersChunk &chunk);

    Platform::Signal<std::vector<Snowflake>> presencesChanged;

private:
    bool apply(const Discord::Presence &presence, Snowflake guildId);
    void ingestMember(const Discord::Member &member, Snowflake guildId);
    void dropSlot(Snowflake guildId);
    void rebuildSelfActivities(std::vector<Discord::Activity> sessionActivities);
    void markDirty(Snowflake userId);
    void flushDirty();

    [[nodiscard]] bool hidesGames(Snowflake userId) const;
    [[nodiscard]] const PresenceEntry *resolve(Snowflake userId, Snowflake guildId) const;
    [[nodiscard]] const PresenceEntry *newest(Snowflake userId) const;

    std::unordered_map<Snowflake, std::unordered_map<Snowflake, PresenceEntry>> presences;

    Snowflake selfUserId;
    PresenceEntry selfPresence;
    std::optional<Discord::Activity> selfCustomStatus;
    bool sessionsReplaceSeen = false;
    bool showCurrentGame = true;

    std::unordered_set<Snowflake> dirtyUsers;
    Platform::Timer flushTimer;
};

} // namespace Core
} // namespace Styx
