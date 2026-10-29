#include "MemberListModel.hpp"

#include "Core/Presence/ActivityFormat.hpp"
#include "Discord/CdnUrls.hpp"

#include <unordered_set>

namespace Styx::UI {

namespace {
constexpr Platform::Size AvatarRequestSize{32, 32};
constexpr Platform::Size EmojiRequestSize{16, 16};
}

MemberListModel::MemberListModel(Core::ImageManager *images) : imageManager(images)
{
    if (imageManager) {
        track(imageManager->imageFetched.connect([this](const std::string &url, const Platform::Size &, const Platform::ImageBuffer &) {
            avatarTracker.notify(url, [this](Core::Snowflake userId) {
                if (manager)
                    notifyRows(manager->indicesForUsers({userId}));
            });
        }));
    }
}

void MemberListModel::setManager(Core::MemberListManager *newManager)
{
    managerConnections.clear();
    manager = newManager;
    avatarTracker.clear();
    if (manager) {
        managerConnections.push_back(manager->listAboutToReset.connect([this] { avatarTracker.clear(); }));
        managerConnections.push_back(manager->listReset.connect([this] { reset.publish(); }));
        managerConnections.push_back(manager->itemsChanged.connect([this](const std::vector<int> &indices) { notifyRows(indices); }));
    }
    reset.publish();
}

void MemberListModel::setPresenceManager(Core::PresenceManager *presences)
{
    presenceConnection.disconnect();
    presenceManager = presences;
    if (presenceManager) {
        presenceConnection = presenceManager->presencesChanged.connect([this](const std::vector<Core::Snowflake> &ids) {
            if (manager)
                notifyRows(manager->indicesForUsers(std::unordered_set<Core::Snowflake>(ids.begin(), ids.end())));
        });
    }
    reset.publish();
}

void MemberListModel::setAccount(Core::Snowflake id) { accountId = id; }

int MemberListModel::rowCount() const { return manager ? manager->totalItemCount() : 0; }

MemberRow MemberListModel::row(int index) const
{
    MemberRow result;
    if (!manager || index < 0 || index >= manager->totalItemCount())
        return result;
    const auto *item = manager->itemAt(index);
    if (!item)
        return result;
    result.loaded = true;
    result.type = item->type;
    if (item->type == Core::MemberListItem::Type::Group) {
        result.groupName = item->groupName;
        result.groupCount = item->groupCount;
        result.groupColor = item->groupColor;
    } else if (item->type == Core::MemberListItem::Type::Member) {
        result.userId = item->userId;
        result.username = item->displayName;
        result.roleColor = item->roleColor;
        const auto &member = item->member;
        const std::string userAvatar = member.user.hasValue() ? member.user->avatar.valueOr() : std::string{};
        result.avatar = cachedImage(Discord::Cdn::effectiveAvatar(manager->currentGuildId(), item->userId,
                                    member.avatar.valueOr(), userAvatar, AvatarRequestSize.width), AvatarRequestSize, item->userId);
        if (presenceManager) {
            const auto guildId = manager->currentGuildId();
            result.presence = presenceManager->badge(item->userId, guildId);
            if (const auto *activity = presenceManager->primaryActivity(item->userId, guildId)) {
                result.activity.text = Core::ActivityFormat::secondaryText(*activity);
                result.activity.kind = activity->kind();
                result.hasActivity = !result.activity.text.empty();
                if (activity->isCustom() && activity->emoji.hasValue()) {
                    const auto &emoji = activity->emoji.get();
                    if (emoji.isUnicode())
                        result.activity.emojiText = emoji.nameText();
                    else
                        result.activity.emoji = cachedImage(emoji.stillImageUrl(EmojiRequestSize.width), EmojiRequestSize, item->userId);
                }
            }
        }
    }
    return result;
}

Platform::ImageBuffer MemberListModel::cachedImage(const std::string &url, Platform::Size size, Core::Snowflake userId) const
{
    if (url.empty() || !imageManager)
        return {};
    if (imageManager->isCached(url, size))
        return imageManager->get(url, size, accountId);
    imageManager->get(url, size, accountId);
    avatarTracker.track(url, userId);
    return {};
}

void MemberListModel::notifyRows(const std::vector<int> &rows)
{
    if (!rows.empty())
        rowsChanged.publish(rows);
}

} // namespace Styx::UI
