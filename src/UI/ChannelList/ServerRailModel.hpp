#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Image.hpp"
#include "Platform/Trackable.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Styx::Core { class Session; }
namespace Styx::UI {
class ChannelTreeModel;
struct ChannelNode;

class ServerRailModel : public Platform::Trackable
{
public:
    enum class Kind { AccountHome, Folder, Server };
    struct RailEntry {
        Kind kind = Kind::AccountHome;
        Core::Snowflake accountId;
        Core::Snowflake id;
        std::string name;
        bool isFirstOfAccount = false;
        int depth = 0;
        bool inFolderGroup = false;
        std::optional<std::uint64_t> folderColor;
        std::string accountAvatarHash;
        std::vector<Platform::ImageBuffer> folderIcons;
        ChannelNode *node = nullptr;
        ChannelNode *unreadNode = nullptr;
    };

    ServerRailModel(Core::Session *session, ChannelTreeModel *source);
    const std::vector<RailEntry> &rows() const { return entries; }
    bool isUnread(const RailEntry &entry) const;
    int mentionCount(const RailEntry &entry) const;
    bool isMuted(const RailEntry &entry) const;
    bool isExpanded(const RailEntry &entry) const;
    bool isSelected(const RailEntry &entry) const;
    Core::Snowflake ownerId(const RailEntry &entry) const;
    Platform::ImageBuffer decorationFor(const RailEntry &entry) const;
    void toggleFolder(Core::Snowflake accountId, Core::Snowflake folderId);
    bool isFolderExpanded(Core::Snowflake accountId, Core::Snowflake folderId) const;
    void setSelected(Kind kind, Core::Snowflake accountId, Core::Snowflake id);
    std::vector<std::string> expandedFolderKeys() const;
    void setExpandedFolderKeys(const std::vector<std::string> &keys);
    void rebuild();
    Platform::Signal<> changed;
    Platform::Signal<> reset;

private:
    static std::string folderKey(Core::Snowflake accountId, Core::Snowflake folderId);
    Core::Snowflake folderForGuild(Core::Snowflake accountId, Core::Snowflake guildId) const;
    std::vector<Platform::ImageBuffer> computeFolderChildIcons(ChannelNode *folder) const;
    Core::Session *session;
    ChannelTreeModel *source;
    std::vector<RailEntry> entries;
    std::unordered_set<std::string> expandedFolders;
    Kind selectedKind = Kind::AccountHome;
    Core::Snowflake selectedAccountId;
    Core::Snowflake selectedGuildId;
    Core::Snowflake selectedFolderId;
    mutable std::unordered_set<std::string> pendingAccountAvatars;
};
}
