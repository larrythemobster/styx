#pragma once

#include <string>
#include <vector>

#include "Core/MemberListManager.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Platform/Trackable.hpp"
#include "UI/AvatarRequestTracker.hpp"

namespace Styx::UI {

struct MemberActivity
{
    std::string text;
    Discord::ActivityType kind = Discord::ActivityType::PLAYING;
    Platform::ImageBuffer emoji;
    std::string emojiText;
};

struct MemberRow
{
    Core::MemberListItem::Type type = Core::MemberListItem::Type::Placeholder;
    bool loaded = false;
    Core::Snowflake userId;
    std::string username;
    Platform::ImageBuffer avatar;
    Platform::Color roleColor;
    std::string groupName;
    int groupCount = 0;
    Platform::Color groupColor;
    Core::PresenceBadge presence;
    MemberActivity activity;
    bool hasActivity = false;
};

class MemberListModel : public Platform::Trackable
{
public:
    explicit MemberListModel(Core::ImageManager *imageManager);
    void setManager(Core::MemberListManager *manager);
    void setPresenceManager(Core::PresenceManager *presences);
    void setAccount(Core::Snowflake accountId);
    [[nodiscard]] int rowCount() const;
    [[nodiscard]] MemberRow row(int index) const;

    Platform::Signal<> reset;
    Platform::Signal<std::vector<int>> rowsChanged;

private:
    Platform::ImageBuffer cachedImage(const std::string &url, Platform::Size size, Core::Snowflake userId) const;
    void notifyRows(const std::vector<int> &rows);

    Platform::Guard<Core::MemberListManager> manager;
    Platform::Guard<Core::PresenceManager> presenceManager;
    mutable AvatarRequestTracker<Core::Snowflake> avatarTracker;
    Core::ImageManager *imageManager;
    Core::Snowflake accountId;
    std::vector<Platform::Connection> managerConnections;
    Platform::Connection presenceConnection;
};

} // namespace Styx::UI
