#pragma once

#include <unordered_map>
#include <vector>
#include "Platform/Trackable.hpp"
#include "Core/Presence/PresenceBadge.hpp"

#include "ChannelNode.hpp"
#include "Core/AccountInfo.hpp"

#include "Core/Snowflake.hpp"
#include "Core/Session.hpp"
#include "Discord/Events.hpp"
#include "UI/AvatarRequestTracker.hpp"

using Styx::Core::Session;
using Styx::Core::Snowflake;

namespace Styx::Core {
class ClientInstance;
} // namespace Styx::Core

namespace Styx::Proto {
struct GuildFolder;
} // namespace Styx::Proto

namespace Styx {
namespace UI {
class ChannelTreeModel : public Platform::Trackable
{
public:
    explicit ChannelTreeModel(Session *session);
    ChannelNode *rootNode() const { return root.get(); }
    Platform::ImageBuffer avatar(ChannelNode *node) const;
    Core::PresenceBadge presenceBadge(ChannelNode *node) const;
    bool threadJoined(ChannelNode *node) const;
    bool voiceMuted(ChannelNode *node) const;
    bool voiceDeafened(ChannelNode *node) const;

    void addAccount(const Styx::Core::AccountInfo &account);
    void removeAccount(Snowflake accountId);

    void populateFromReady(const Discord::Ready &ready);

    static ChannelNode *getAccountNodeFor(ChannelNode *node);
    static ChannelNode *findGuildNode(ChannelNode *node);

    void addGuild(const Discord::GatewayGuild &guild, Snowflake accountId);
    void updateGuild(const Discord::Guild &guild, Snowflake accountId);
    void removeGuild(Snowflake accountId, Snowflake guildId);
    void addChannel(const Discord::ChannelCreate &event, Snowflake accountId);
    void updateChannel(const Discord::ChannelUpdate &update, Snowflake accountId);
    void deleteChannel(const Discord::ChannelDelete &event, Snowflake accountId);
    void addThread(const Discord::Channel &thread, Snowflake accountId);
    void updateThread(const Discord::Channel &thread, Snowflake accountId);
    void removeThread(Snowflake threadId, Snowflake accountId);
    void syncThreads(Snowflake guildId, const std::vector<Snowflake> &parentIds, const std::vector<Discord::Channel> &threads, Snowflake accountId);
    void showTemporaryThread(const Discord::Channel &thread, Snowflake accountId);
    void clearTemporaryThread(Snowflake exceptThreadId = Snowflake::Invalid);
    void promoteTemporaryThread(Snowflake threadId);
    void invalidateGuildData(Snowflake guildId);
    void updateReadState(Snowflake channelId, Snowflake accountId);
    void updatePresence(Snowflake accountId, const std::vector<Snowflake> &userIds);
    void updateForumBadge(Snowflake forumId, Snowflake accountId);
    void updateForumThreads(Snowflake forumId, Snowflake accountId);
    void updateGuildSettings(Snowflake guildId, Snowflake accountId);
    void updateChannelLastMessageId(Snowflake channelId, Snowflake messageId, Snowflake accountId);
    void updateVoiceCount(Snowflake channelId, int count, Snowflake accountId);
    void setAccountVoiceChannel(Snowflake accountId, Snowflake channelId);
    void updateVoiceParticipant(Snowflake channelId, Snowflake userId, bool joined, Snowflake accountId);
    void updateVoiceParticipantState(Snowflake channelId, Snowflake userId, Snowflake accountId);
    void toggleCollapsed(ChannelNode *node);
    void setCollapsed(ChannelNode *node, bool collapsed);

    std::vector<Snowflake> getMarkableChannelIds(ChannelNode *node);
    ChannelNode *findChannelTreeNode(Snowflake channelId);
    ChannelNode *findChannelTreeNode(Snowflake channelId, Snowflake accountId);

    ChannelNode *serverNode(Snowflake accountId, Snowflake guildId);
    ChannelNode *folderNode(Snowflake accountId, Snowflake folderId);
    ChannelNode *dmHeaderNode(Snowflake accountId);

public:
    Platform::Signal<ChannelNode *> changed;
    Platform::Signal<> reset;
    Platform::Signal<int, int> totalMentionCountChanged;

private:
    static void collectMarkableChannelIds(ChannelNode *node, std::vector<Snowflake> &out);
    static Core::ChannelReadState computeNodeReadState(ChannelNode *node, Core::ClientInstance *instance);
    static Core::ChannelReadState shownReadState(const ChannelNode *node);
    static void setSelfReadState(ChannelNode *node, const Core::ChannelReadState &state);
    static void aggregateChildren(ChannelNode *node);
    void refreshReadStates(ChannelNode *node, Core::ClientInstance *instance);
    bool notifyIfReadStateChanged(ChannelNode *node, const Core::ChannelReadState &before);
    void updateNodeAggregates(ChannelNode *node);
    void publishTotalMentionCount();
    std::unique_ptr<ChannelNode> createGuildNode(const Discord::GatewayGuild &guild, Core::ClientInstance *instance);
    static std::unique_ptr<ChannelNode> makeThreadNode(const Discord::Channel &thread);
    ChannelNode *insertThreadNode(const Discord::Channel &thread, Snowflake accountId, bool temporary = false);
    bool removeChildRow(ChannelNode *parent, ChannelNode *node);
    void resortThread(ChannelNode *node);
    std::unique_ptr<ChannelNode> createFolderNode(const Proto::GuildFolder &folder);
    void placeGuildNode(ChannelNode *accNode, Snowflake guildId, std::unique_ptr<ChannelNode> guildNode, Core::ClientInstance *instance);
    ChannelNode *findChannelTreeNode(Snowflake channelId, ChannelNode *root);
    ChannelNode *findGuildNodeById(Snowflake guildId, ChannelNode *accountNode);
    ChannelNode *findFolderNodeById(ChannelNode *accountNode, Snowflake folderId);
    ChannelNode *findCategoryNode(Snowflake categoryId, ChannelNode *guildNode);
    ChannelNode *accountNode(Snowflake accountId) const;
    void insertChildAt(ChannelNode *parent, int row, std::unique_ptr<ChannelNode> node);
    void emitDataChangedRecursive(ChannelNode *node);

private:
    Session *session;

    std::unique_ptr<ChannelNode> root;
    std::unordered_map<Snowflake, ChannelNode *> accountNodes;
    std::unordered_map<Snowflake, Snowflake> accountVoiceChannels;
    int publishedMentionCount = 0;
    mutable AvatarRequestTracker<std::uint64_t> avatarTracker;
    mutable std::uint64_t nextAvatarTarget = 1;

    Snowflake temporaryThreadId;
    Snowflake temporaryThreadAccount;
};
} // namespace UI
} // namespace Styx
