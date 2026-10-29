#include "ServerRailModel.hpp"
#include "ChannelNode.hpp"
#include "ChannelTreeModel.hpp"
#include "Core/AccountInfo.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Session.hpp"
#include "Discord/CdnUrls.hpp"

namespace Styx::UI {
using Core::Snowflake;

ServerRailModel::ServerRailModel(Core::Session *session, ChannelTreeModel *source)
    : session(session), source(source)
{
    track(source->reset.connect([this] { rebuild(); }));
    track(source->changed.connect([this](ChannelNode *node) {
        for (auto &entry : entries) {
            if (entry.node == node) entry.name = node->name;
            if (entry.kind == Kind::Folder && node && node->parent == entry.node)
                entry.folderIcons = computeFolderChildIcons(entry.node);
        }
        changed.publish();
    }));
    track(session->accountDetailsUpdated.connect([this](const Core::AccountInfo &info) {
        for (auto &entry : entries) {
            if (entry.kind == Kind::AccountHome && entry.accountId == info.id) {
                entry.name = info.displayName.empty() ? info.username : info.displayName;
                entry.accountAvatarHash = info.avatar;
            }
        }
        changed.publish();
    }));
    track(session->getImageManager()->imageFetched.connect(
        [this](const std::string &url, const Platform::Size &, const Platform::ImageBuffer &) {
            if (pendingAccountAvatars.erase(url)) changed.publish();
        }));
    rebuild();
}

bool ServerRailModel::isUnread(const RailEntry &entry) const { return entry.unreadNode && entry.unreadNode->isUnread; }
int ServerRailModel::mentionCount(const RailEntry &entry) const { return entry.unreadNode ? entry.unreadNode->mentionCount : 0; }
bool ServerRailModel::isMuted(const RailEntry &entry) const { return entry.unreadNode && entry.unreadNode->isMuted; }
bool ServerRailModel::isExpanded(const RailEntry &entry) const { return entry.kind == Kind::Folder && isFolderExpanded(entry.accountId, entry.id); }
bool ServerRailModel::isSelected(const RailEntry &entry) const
{
    if (entry.kind == Kind::AccountHome) return selectedKind == Kind::AccountHome && entry.accountId == selectedAccountId;
    if (entry.kind == Kind::Server) return selectedKind == Kind::Server && entry.accountId == selectedAccountId && entry.id == selectedGuildId;
    return selectedKind == Kind::Server && entry.accountId == selectedAccountId && entry.id == selectedFolderId && !isExpanded(entry);
}
Snowflake ServerRailModel::ownerId(const RailEntry &entry) const
{
    return entry.kind == Kind::Server && entry.node ? entry.node->ownerId : Snowflake();
}
Platform::ImageBuffer ServerRailModel::decorationFor(const RailEntry &entry) const
{
    if (entry.kind == Kind::Server) return entry.node ? source->avatar(entry.node) : Platform::ImageBuffer{};
    if (entry.kind != Kind::AccountHome || entry.accountAvatarHash.empty()) return {};
    Platform::Size size{64, 64};
    auto url = Discord::Cdn::userAvatar(entry.accountId, entry.accountAvatarHash, size.width);
    if (url.empty()) return {};
    auto *images = session->getImageManager();
    auto image = images->get(url, size, entry.accountId, Core::PinGroup::ChannelList);
    if (!images->isCached(url, size)) {
        pendingAccountAvatars.insert(url);
        return {};
    }
    return image;
}
std::vector<Platform::ImageBuffer> ServerRailModel::computeFolderChildIcons(ChannelNode *folder) const
{
    std::vector<Platform::ImageBuffer> icons;
    if (!folder) return icons;
    for (const auto &child : folder->children) {
        if (child->type != ChannelNode::Type::Server) continue;
        icons.push_back(child->TEMP_iconHash.empty() ? Platform::ImageBuffer{} : source->avatar(child.get()));
        if (icons.size() == 4) break;
    }
    return icons;
}
void ServerRailModel::rebuild()
{
    entries.clear();
    for (const auto &account : source->rootNode()->children) {
        if (account->type != ChannelNode::Type::Account) continue;
        RailEntry home;
        home.kind = Kind::AccountHome;
        home.accountId = home.id = account->id;
        home.name = account->name;
        home.isFirstOfAccount = true;
        home.node = account.get();
        home.accountAvatarHash = session->getAccountInfo(account->id).avatar;
        for (const auto &child : account->children)
            if (child->type == ChannelNode::Type::DMHeader) { home.unreadNode = child.get(); break; }
        entries.push_back(std::move(home));
        for (const auto &child : account->children) {
            if (child->type == ChannelNode::Type::Server) {
                RailEntry entry;
                entry.kind = Kind::Server; entry.accountId = account->id; entry.id = child->id;
                entry.name = child->name; entry.node = entry.unreadNode = child.get();
                entries.push_back(std::move(entry));
            } else if (child->type == ChannelNode::Type::Folder) {
                bool expanded = isFolderExpanded(account->id, child->id);
                RailEntry folder;
                folder.kind = Kind::Folder; folder.accountId = account->id; folder.id = child->id;
                folder.name = child->name; folder.node = folder.unreadNode = child.get();
                folder.folderColor = child->folderColor; folder.inFolderGroup = expanded;
                folder.folderIcons = computeFolderChildIcons(child.get());
                entries.push_back(std::move(folder));
                if (expanded) for (const auto &guild : child->children) {
                    if (guild->type != ChannelNode::Type::Server) continue;
                    RailEntry entry;
                    entry.kind = Kind::Server; entry.accountId = account->id; entry.id = guild->id;
                    entry.name = guild->name; entry.depth = 1; entry.node = entry.unreadNode = guild.get();
                    entry.folderColor = child->folderColor; entry.inFolderGroup = true;
                    entries.push_back(std::move(entry));
                }
            }
        }
    }
    selectedFolderId = selectedKind == Kind::Server ? folderForGuild(selectedAccountId, selectedGuildId) : Snowflake();
    reset.publish();
}
void ServerRailModel::toggleFolder(Snowflake accountId, Snowflake folderId)
{
    auto key = folderKey(accountId, folderId);
    if (!expandedFolders.erase(key)) expandedFolders.insert(std::move(key));
    rebuild();
}
bool ServerRailModel::isFolderExpanded(Snowflake accountId, Snowflake folderId) const
{
    return expandedFolders.contains(folderKey(accountId, folderId));
}
void ServerRailModel::setSelected(Kind kind, Snowflake accountId, Snowflake id)
{
    auto guild = kind == Kind::Server ? id : Snowflake();
    if (selectedKind == kind && selectedAccountId == accountId && selectedGuildId == guild) return;
    selectedKind = kind; selectedAccountId = accountId; selectedGuildId = guild;
    selectedFolderId = kind == Kind::Server ? folderForGuild(accountId, id) : Snowflake();
    changed.publish();
}
Snowflake ServerRailModel::folderForGuild(Snowflake accountId, Snowflake guildId) const
{
    for (const auto &account : source->rootNode()->children) {
        if (account->type != ChannelNode::Type::Account || account->id != accountId) continue;
        for (const auto &folder : account->children) {
            if (folder->type != ChannelNode::Type::Folder) continue;
            for (const auto &guild : folder->children)
                if (guild->type == ChannelNode::Type::Server && guild->id == guildId) return folder->id;
        }
        break;
    }
    return {};
}
std::vector<std::string> ServerRailModel::expandedFolderKeys() const
{
    return {expandedFolders.begin(), expandedFolders.end()};
}
void ServerRailModel::setExpandedFolderKeys(const std::vector<std::string> &keys)
{
    expandedFolders = std::unordered_set<std::string>(keys.begin(), keys.end());
    rebuild();
}
std::string ServerRailModel::folderKey(Snowflake accountId, Snowflake folderId)
{
    return accountId.toString() + ":" + folderId.toString();
}
}
