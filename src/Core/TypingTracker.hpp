#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "Platform/Trackable.hpp"
#include "Platform/Timer.hpp"

#include "Snowflake.hpp"

namespace Styx {
namespace Core {

class UserManager;

struct TyperInfo
{
    Snowflake userId;
    std::optional<Snowflake> guildId;
    std::string name;
};

class TypingTracker : public Platform::Trackable
{
public:
    TypingTracker();

    void setUserManager(UserManager *manager);
    void setActiveChannel(Snowflake channelId);
    void setCurrentUserId(Snowflake userId);

    void addTyper(Snowflake channelId, Snowflake userId, std::optional<Snowflake> guildId);
    void removeTyper(Snowflake channelId, Snowflake userId);
    void clear();

    std::vector<TyperInfo> getActiveTypers() const;

    Platform::Signal<> typersChanged;

private:
    void cleanupExpired();

private:
    struct TypingEntry
    {
        Snowflake userId;
        std::optional<Snowflake> guildId;
        std::chrono::steady_clock::time_point expiresAt;
    };

    std::unordered_map<Snowflake, std::vector<TypingEntry>> channelTypers;
    Snowflake activeChannel;
    Snowflake currentUserId;
    UserManager *userManager = nullptr;
    Platform::Timer cleanupTimer;
};

} // namespace Core
} // namespace Styx
