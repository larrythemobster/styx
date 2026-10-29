#include "TypingTracker.hpp"
#include "UserManager.hpp"
#include <algorithm>

namespace Styx {
namespace Core {

TypingTracker::TypingTracker()
{
    cleanupTimer.start(std::chrono::seconds(1), true, [this] { cleanupExpired(); });
}

void TypingTracker::setUserManager(UserManager *manager)
{
    userManager = manager;
}

void TypingTracker::setActiveChannel(Snowflake channelId)
{
    if (activeChannel != channelId) {
        activeChannel = channelId;
        typersChanged();
    }
}

void TypingTracker::setCurrentUserId(Snowflake userId)
{
    currentUserId = userId;
}

void TypingTracker::addTyper(Snowflake channelId, Snowflake userId,
                             std::optional<Snowflake> guildId)
{
    if (userId == currentUserId)
        return;

    auto expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    TypingEntry entry{ userId, guildId, expiresAt };

    auto &typers = channelTypers[channelId];

    bool found = false;
    for (auto &t : typers) {
        if (t.userId == userId) {
            t.expiresAt = expiresAt;
            found = true;
            break;
        }
    }
    if (!found) {
        typers.push_back(entry);
    }

    if (channelId == activeChannel) {
        typersChanged();
    }
}

void TypingTracker::removeTyper(Snowflake channelId, Snowflake userId)
{
    auto &typers = channelTypers[channelId];
    auto it = std::find_if(typers.begin(), typers.end(),
                           [userId](const TypingEntry &t) { return t.userId == userId; });
    if (it != typers.end()) {
        typers.erase(it);
        typersChanged();
    }
}

void TypingTracker::clear()
{
    channelTypers.clear();
    typersChanged();
}

std::vector<TyperInfo> TypingTracker::getActiveTypers() const
{
    if (!userManager || !activeChannel.isValid())
        return {};

    auto it = channelTypers.find(activeChannel);
    if (it == channelTypers.end() || it->second.empty())
        return {};

    std::vector<TyperInfo> typers;
    for (const auto &entry : it->second) {
        TyperInfo info;
        info.userId = entry.userId;
        info.guildId = entry.guildId;
        info.name = userManager->getDisplayName(entry.userId, entry.guildId.value_or(Snowflake::Invalid));
        typers.push_back(std::move(info));
    }
    return typers;
}

void TypingTracker::cleanupExpired()
{
    auto now = std::chrono::steady_clock::now();
    bool activeChannelChanged = false;

    for (auto it = channelTypers.begin(); it != channelTypers.end();) {
        auto &list = it->second;
        auto before = list.size();

        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&now](const TypingEntry &e) { return e.expiresAt <= now; }),
                   list.end());

        if (list.size() != before && it->first == activeChannel)
            activeChannelChanged = true;

        if (list.empty()) {
            it = channelTypers.erase(it);
        } else {
            ++it;
        }
    }

    if (activeChannelChanged)
        typersChanged();
}

} // namespace Core
} // namespace Styx
