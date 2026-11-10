#include "ChannelTreeModel.hpp"

#include <algorithm>
#include <functional>
#include <unordered_set>
#include "Platform/Native.hpp"
#include <utility>

#include <Core/ClientInstance.hpp>
#include <Core/ForumManager.hpp>
#include <Core/Presence/PresenceBadge.hpp>
#include <Core/ReadStateManager.hpp>
#include <Core/Logging.hpp>
#include <Discord/CdnUrls.hpp>
#include <Discord/Enums.hpp>
#include <Storage/DatabaseManager.hpp>
#include <Storage/ChannelRepository.hpp>
#include <Storage/UserRepository.hpp>

#ifndef STYX_NO_VOICE
#include <Core/Audio/VoiceManager.hpp>
#endif

namespace Styx {
namespace UI {

constexpr static int VoiceParticipantAvatarPx = 32;

static bool isChannelPrivate(const Discord::Channel &channel, Core::Snowflake guildId)
{
    if (!channel.permissionOverwrites.hasValue())
        return false;

    for (const auto &ow : channel.permissionOverwrites.get()) {
        if (ow.type.get() == Discord::PermissionOverwrite::Type::Role && ow.id.get() == guildId)
            return ow.deny->testFlag(Discord::Permission::VIEW_CHANNEL) ||
                   (channel.isVoice() && ow.deny->testFlag(Discord::Permission::CONNECT));
    }
    return false;
}

static std::string getDMDisplayName(const Discord::Channel &channel, Core::UserManager *users)
{
    if (channel.name.hasValue() && !channel.name->empty())
        return channel.name.get();
    std::vector<std::string> names;
    if (channel.recipients.hasValue()) {
        for (const auto &user : channel.recipients.get()) {
            auto nickname = users->getNickname(user.id.get(), Core::Snowflake::Invalid);
            names.push_back(nickname.empty() ? user.getDisplayName() : nickname);
        }
    } else if (channel.recipientIds.hasValue()) {
        for (const auto &id : channel.recipientIds.get())
            if (users->getUser(id)) names.push_back(users->getDisplayName(id));
    }
    if (names.empty()) return "Unnamed";
    std::string result;
    for (const auto &name : names) {
        if (!result.empty()) result += ", ";
        result += name;
    }
    return result;
}

static void updatePrivateChannelNode(ChannelNode &node, const Discord::Channel &channel, Core::UserManager *users)
{
    node.name = getDMDisplayName(channel, users);
    node.lastMessageId = channel.lastMessageId.hasValue() ? channel.lastMessageId.get() : channel.id.get();
    node.recipientIds.clear();
    if (channel.recipients.hasValue()) {
        node.recipientIds.reserve(channel.recipients->size());
        for (const auto &user : channel.recipients.get())
            node.recipientIds.push_back(user.id.get());
    } else if (channel.recipientIds.hasValue()) {
        node.recipientIds = channel.recipientIds.get();
    }
    node.dmRecipientId = Snowflake::Invalid;
    node.dmAvatarHash.clear();
    node.TEMP_iconHash.clear();
    if (channel.type == Discord::ChannelType::DM && node.recipientIds.size() == 1) {
        node.dmRecipientId = node.recipientIds.front();
        if (auto user = users->getUser(node.dmRecipientId); user && user->avatar.hasValue())
            node.dmAvatarHash = user->avatar.get();
    } else if (channel.type == Discord::ChannelType::GROUP_DM && channel.icon.hasValue()) {
        node.TEMP_iconHash = channel.icon.get();
    }
}

static std::optional<ChannelNode::Type> nodeTypeForChannel(Discord::ChannelType type)
{
    switch (type) {
    case Discord::ChannelType::GUILD_TEXT:
    case Discord::ChannelType::GUILD_NEWS:
        return ChannelNode::Type::Channel;
    case Discord::ChannelType::GUILD_VOICE:
    case Discord::ChannelType::GUILD_STAGE_VOICE:
        return ChannelNode::Type::VoiceChannel;
    case Discord::ChannelType::GUILD_FORUM:
        return ChannelNode::Type::Forum;
    case Discord::ChannelType::GUILD_CATEGORY:
        return ChannelNode::Type::Category;
    default:
        return std::nullopt;
    }
}

ChannelTreeModel::ChannelTreeModel(Session *session) : session(session)
{
    root = std::make_unique<ChannelNode>();
    root->name = "Root";
    root->type = ChannelNode::Type::Root;
    track(session->accountDetailsUpdated.connect([this](const Core::AccountInfo &info) {
        auto it = accountNodes.find(info.id);
        if (it == accountNodes.end()) return;
        it->second->name = info.displayName;
        changed.publish(it->second);
    }));
    track(session->getImageManager()->imageFetched.connect(
        [this](const std::string &url, const Platform::Size &, const Platform::ImageBuffer &) {
            avatarTracker.notify(url, [this](std::uint64_t target) {
                std::function<ChannelNode *(ChannelNode *)> find = [&](ChannelNode *node) -> ChannelNode * {
                    if (node->avatarTarget == target) return node;
                    for (const auto &child : node->children)
                        if (auto *found = find(child.get())) return found;
                    return nullptr;
                };
                if (auto *node = find(root.get())) changed.publish(node);
            });
        }));
}

Platform::ImageBuffer ChannelTreeModel::avatar(ChannelNode *node) const
{
    if (!node) return {};
    auto *account = getAccountNodeFor(node);
    auto accountId = account ? account->id : Snowflake();
    Platform::Size size{64, 64};
    std::string url;
    if (node->type == ChannelNode::Type::Server)
        url = Discord::Cdn::guildIcon(node->id, node->TEMP_iconHash, size.width);
    else if (node->type == ChannelNode::Type::DMChannel) {
        if (node->dmRecipientId.isValid())
            url = Discord::Cdn::userAvatar(node->dmRecipientId, node->dmAvatarHash, size.width);
        else if (!node->TEMP_iconHash.empty())
            url = Discord::Cdn::channelIcon(node->id, node->TEMP_iconHash, size.width);
    } else if (node->type == ChannelNode::Type::VoiceParticipant && node->dmRecipientId.isValid()) {
        size = {VoiceParticipantAvatarPx, VoiceParticipantAvatarPx};
        url = node->voiceParticipantAvatarUrl;
    }
    if (url.empty()) return {};
    if (!node->avatarTarget) node->avatarTarget = nextAvatarTarget++;
    return avatarTracker.fetch(session->getImageManager(), url, size, node->avatarTarget,
                               accountId, Core::PinGroup::ChannelList);
}

Core::PresenceBadge ChannelTreeModel::presenceBadge(ChannelNode *node) const
{
    if (!node || (node->type != ChannelNode::Type::DMChannel && node->type != ChannelNode::Type::VoiceParticipant) || !node->dmRecipientId.isValid()) return {};
    auto *account = getAccountNodeFor(node);
    auto *instance = account ? session->client(account->id) : nullptr;
    return instance ? instance->presences()->badge(node->dmRecipientId) : Core::PresenceBadge();
}

bool ChannelTreeModel::threadJoined(ChannelNode *node) const
{
    if (!node || node->type != ChannelNode::Type::Thread || !node->parent || node->parent->type != ChannelNode::Type::Channel) return false;
    auto *account = getAccountNodeFor(node);
    auto *instance = account ? session->client(account->id) : nullptr;
    return instance && instance->isThreadJoined(node->id);
}

bool ChannelTreeModel::voiceMuted(ChannelNode *node) const
{
#ifndef STYX_NO_VOICE
    if (node && node->type == ChannelNode::Type::VoiceParticipant) {
        auto *account = getAccountNodeFor(node);
        auto *instance = account ? session->client(account->id) : nullptr;
        if (instance) { auto state = instance->voice()->voiceStateForUser(node->id); return state && (state->selfMute.get() || state->mute.get()); }
    }
#endif
    return false;
}

bool ChannelTreeModel::voiceDeafened(ChannelNode *node) const
{
#ifndef STYX_NO_VOICE
    if (node && node->type == ChannelNode::Type::VoiceParticipant) {
        auto *account = getAccountNodeFor(node);
        auto *instance = account ? session->client(account->id) : nullptr;
        if (instance) { auto state = instance->voice()->voiceStateForUser(node->id); return state && (state->selfDeaf.get() || state->deaf.get()); }
    }
#endif
    return false;
}

void ChannelTreeModel::addAccount(const Styx::Core::AccountInfo &account)
{
    if (accountNodes.contains(account.id))
        return;

    auto accNode = std::make_unique<ChannelNode>();
    accNode->id = account.id;
    accNode->name = account.displayName.empty() ? account.username : account.displayName;
    accNode->type = ChannelNode::Type::Account;

    auto dmNode = std::make_unique<ChannelNode>();
    dmNode->name = "Direct Messages";
    dmNode->type = ChannelNode::Type::DMHeader;
    accNode->addChild(std::move(dmNode));

    accountNodes[account.id] = root->addChild(std::move(accNode));
    reset.publish();

}

void ChannelTreeModel::removeAccount(Snowflake accountId)
{
    if (!accountNodes.contains(accountId))
        return;

    ChannelNode *nodeToRemove = accountNodes.at(accountId);
    accountNodes.erase(accountId);
    accountVoiceChannels.erase(accountId);
    removeChildRow(root.get(), nodeToRemove);
    publishTotalMentionCount();
}

void ChannelTreeModel::populateFromReady(const Discord::Ready &ready)
{
    ChannelNode *accNode = accountNode(ready.user->id);
    if (!accNode)
        return;

    auto *instance = session->client(ready.user->id);
    if (!instance)
        return;

    const auto &settings = instance->discord()->getSettings();

    std::unordered_map<Core::Snowflake, const Discord::GatewayGuild *> guildMap;
    for (const auto &guild : ready.guilds.get())
        guildMap.emplace(guild.properties->id.get(), &guild);

    // folders and unfolder'd guilds
    std::vector<std::unique_ptr<ChannelNode>> topLevelNodes;

    if (settings.guildFolders.has_value() && !settings.guildFolders->folders.empty()) {
        const auto &folders = settings.guildFolders->folders;

        std::unordered_set<Core::Snowflake> guildIdsInFolders;
        for (const auto &folder : folders)
            for (const auto &guildId : folder.guildIds)
                guildIdsInFolders.insert(guildId);

        const auto &guilds = ready.guilds.get();
        for (auto it = guilds.rbegin(); it != guilds.rend(); ++it)
            if (!guildIdsInFolders.contains(it->properties->id))
                topLevelNodes.push_back(createGuildNode(*it, instance));

        for (const auto &folder : folders) {
            if (!folder.id.has_value()) {
                // folders with null ids are just guilds
                for (const auto &guildId : folder.guildIds)
                    if (guildMap.contains(guildId))
                        topLevelNodes.push_back(createGuildNode(*guildMap[guildId], instance));
            } else {
                auto folderNode = createFolderNode(folder);

                for (const auto &guildId : folder.guildIds)
                    if (guildMap.contains(guildId))
                        folderNode->addChild(createGuildNode(*guildMap[guildId], instance));

                if (!folderNode->children.empty())
                    topLevelNodes.push_back(std::move(folderNode));
            }
        }
    } else if (settings.guildFolders.has_value() &&
               !settings.guildFolders->guildPositions.empty()) {
        const auto &positions = settings.guildFolders->guildPositions;
        std::unordered_set<Core::Snowflake> positioned;
        for (const auto &id : positions)
            positioned.insert(id);

        const auto &guilds = ready.guilds.get();
        for (auto it = guilds.rbegin(); it != guilds.rend(); ++it)
            if (!positioned.contains(it->properties->id))
                topLevelNodes.push_back(createGuildNode(*it, instance));

        for (const auto &guildId : positions)
            if (guildMap.contains(guildId))
                topLevelNodes.push_back(createGuildNode(*guildMap[guildId], instance));
    } else {
        const auto &guilds = ready.guilds.get();
        for (auto it = guilds.rbegin(); it != guilds.rend(); ++it)
            topLevelNodes.push_back(createGuildNode(*it, instance));
    }

    for (auto &node : topLevelNodes)
        accNode->addChild(std::move(node));

    if (ready.privateChannels.hasValue() && !ready.privateChannels->empty()) {
        ChannelNode *dmHeader = nullptr;
        for (const auto &child : accNode->children) {
            if (child->type == ChannelNode::Type::DMHeader) {
                dmHeader = child.get();
                break;
            }
        }

        if (!dmHeader)
            return;

        Storage::UserRepository userRepo(ready.user->id);

        const auto &dms = ready.privateChannels.get();

        for (const auto &channel : dms) {
            auto dmNode = std::make_unique<ChannelNode>();
            dmNode->id = channel.id;
            dmNode->type = ChannelNode::Type::DMChannel;
            dmNode->name = getDMDisplayName(channel, instance->users());
            dmNode->lastMessageId = channel.lastMessageId.hasValue()
                                            ? channel.lastMessageId.get()
                                            : channel.id.get();

            if (channel.recipients.hasValue()) {
                for (const auto &user : channel.recipients.get())
                    dmNode->recipientIds.push_back(user.id.get());
            } else if (channel.recipientIds.hasValue()) {
                dmNode->recipientIds = channel.recipientIds.get();
            }

            if (channel.type == Discord::ChannelType::DM && dmNode->recipientIds.size() == 1) {
                dmNode->dmRecipientId = dmNode->recipientIds.front();

                if (channel.recipients.hasValue() && !channel.recipients->empty()) {
                    const auto &user = channel.recipients->front();
                    if (user.avatar.hasValue())
                        dmNode->dmAvatarHash = user.avatar.get();
                }

                if (dmNode->dmAvatarHash.empty()) {
                    auto userOpt = userRepo.getUser(dmNode->dmRecipientId);
                    if (userOpt.has_value() && userOpt->avatar.hasValue())
                        dmNode->dmAvatarHash = userOpt->avatar.get();
                }
            } else if (channel.type == Discord::ChannelType::GROUP_DM) {
                if (channel.icon.hasValue())
                    dmNode->TEMP_iconHash = channel.icon.get();
            }

            dmHeader->addChild(std::move(dmNode));
        }
    }

    refreshReadStates(accNode, instance);
    reset.publish();
    publishTotalMentionCount();
}

ChannelNode *ChannelTreeModel::getAccountNodeFor(ChannelNode *node)
{
    ChannelNode *accountNode = node;
    while (accountNode && accountNode->type != ChannelNode::Type::Account)
        accountNode = accountNode->parent;
    return accountNode;
}

std::unique_ptr<ChannelNode> ChannelTreeModel::createGuildNode(const Discord::GatewayGuild &guild, Core::ClientInstance *instance)
{
    auto guildNode = std::make_unique<ChannelNode>();
    guildNode->id = guild.properties->id;
    guildNode->name = guild.properties->name;
    guildNode->type = ChannelNode::Type::Server;
    guildNode->TEMP_iconHash = guild.properties->icon;
    guildNode->ownerId = guild.properties->ownerId;
    guildNode->unavailable = guild.unavailable.hasValue() && guild.unavailable.get();
    if (guild.properties->rulesChannelId.hasValue() && guild.properties->rulesChannelId->isValid())
        guildNode->rulesChannelId = guild.properties->rulesChannelId.get();

    std::unordered_map<Snowflake, ChannelNode *> categoryMap;
    std::unordered_map<Snowflake, ChannelNode *> textChannelMap;
    std::vector<std::unique_ptr<ChannelNode>> categories;
    std::vector<std::unique_ptr<ChannelNode>> orphanChannels;

    for (const auto &channel : guild.channels.get()) {
        if (channel.type == Discord::ChannelType::GUILD_CATEGORY) {
            auto node = std::make_unique<ChannelNode>();
            node->id = channel.id;
            node->name = channel.name;
            node->type = ChannelNode::Type::Category;
            node->position = channel.position;
            node->parentId =
                    channel.parentId.hasValue() ? channel.parentId.get() : Core::Snowflake();
            categoryMap[channel.id] = node.get();
            categories.push_back(std::move(node));
        }
    }

    for (const auto &channel : guild.channels.get()) {
        auto nodeType = nodeTypeForChannel(channel.type);
        if (!nodeType || *nodeType == ChannelNode::Type::Category)
            continue;

        auto node = std::make_unique<ChannelNode>();
        node->id = channel.id;
        node->name = channel.name;
        node->type = *nodeType;
        node->position = channel.position;
        node->parentId = channel.parentId.hasValue() ? channel.parentId.get() : Core::Snowflake();
        node->isPrivate = isChannelPrivate(channel, guild.properties->id);
        node->isAnnouncement = channel.type == Discord::ChannelType::GUILD_NEWS;
        if (*nodeType == ChannelNode::Type::VoiceChannel && channel.userLimit.hasValue())
            node->userLimit = channel.userLimit.get();
        node->lastMessageId = channel.lastMessageId.hasValue() ? channel.lastMessageId.get()
                                                               : Core::Snowflake();

        ChannelNode *rawNode = node.get();
        bool placed = false;
        if (channel.parentId.hasValue() && channel.parentId->isValid()) {
            if (categoryMap.contains(channel.parentId.get())) {
                categoryMap[channel.parentId.get()]->addChild(std::move(node));
                placed = true;
            }
        } else {
            orphanChannels.push_back(std::move(node));
            placed = true;
        }
        if (placed && *nodeType == ChannelNode::Type::Channel)
            textChannelMap.emplace(channel.id.get(), rawNode);
    }

    if (guild.threads.hasValue()) {
        for (const auto &thread : guild.threads.get()) {
            if (thread.isArchived() || !thread.parentId.hasValue())
                continue;
            if (instance && !instance->readState()->isThreadRelevant(thread))
                continue;
            auto it = textChannelMap.find(thread.parentId.get());
            if (it != textChannelMap.end())
                it->second->addChild(makeThreadNode(thread));
        }
        for (auto &[id, channel] : textChannelMap)
            std::sort(channel->children.begin(), channel->children.end(),
                      [](const auto &a, const auto &b) { return a->lastMessageId > b->lastMessageId; });
    }

    auto sorter = [](const auto &a, const auto &b) { return a->position < b->position; };

    std::sort(categories.begin(), categories.end(), sorter);
    std::sort(orphanChannels.begin(), orphanChannels.end(), sorter);
    for (const auto &category : categories)
        std::sort(category->children.begin(), category->children.end(), sorter);

    for (auto &node : orphanChannels)
        guildNode->addChild(std::move(node));
    for (auto &node : categories)
        guildNode->addChild(std::move(node));

    return guildNode;
}

std::unique_ptr<ChannelNode> ChannelTreeModel::makeThreadNode(const Discord::Channel &thread)
{
    auto node = std::make_unique<ChannelNode>();
    node->id = thread.id;
    node->name = thread.name.hasValue() ? thread.name.get() : std::string();
    node->type = ChannelNode::Type::Thread;
    node->parentId = thread.parentId.hasValue() ? thread.parentId.get() : Core::Snowflake();
    node->isArchived = thread.isArchived();
    node->isPrivate = thread.type.hasValue() && thread.type.get() == Discord::ChannelType::PRIVATE_THREAD;
    node->lastMessageId = thread.effectiveLastMessageId();
    return node;
}

ChannelNode *ChannelTreeModel::insertThreadNode(const Discord::Channel &thread, Snowflake accountId, bool temporary)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode || !thread.parentId.hasValue())
        return nullptr;

    if (ChannelNode *existing = findChannelTreeNode(thread.id.get(), accNode))
        return existing;

    ChannelNode *parentChannel = findChannelTreeNode(thread.parentId.get(), accNode);
    if (!parentChannel || parentChannel->type != ChannelNode::Type::Channel)
        return nullptr;

    auto node = makeThreadNode(thread);
    node->isTemporary = temporary;
    ChannelNode *raw = node.get();

    int row = 0;
    for (; row < static_cast<int>(parentChannel->children.size()); ++row)
        if (parentChannel->children[row]->lastMessageId < node->lastMessageId)
            break;

    insertChildAt(parentChannel, row, std::move(node));

    if (auto *instance = session->client(accountId)) {
        setSelfReadState(raw, computeNodeReadState(raw, instance));
        updateNodeAggregates(parentChannel);
    }

    changed.publish(parentChannel);

    return raw;
}

void ChannelTreeModel::addThread(const Discord::Channel &thread, Snowflake accountId)
{
    if (thread.isArchived())
        return;

    auto *instance = session->client(accountId);
    if (!instance || !instance->isThreadJoined(thread.id.get()) ||
        !instance->readState()->isThreadRelevant(thread))
        return;
    insertThreadNode(thread, accountId);
}

void ChannelTreeModel::updateThread(const Discord::Channel &thread, Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *node = findChannelTreeNode(thread.id.get(), accNode);

    if (thread.isArchived()) {
        if (!node)
            return;
        if (!node->isTemporary) {
            removeThread(thread.id.get(), accountId);
            return;
        }
        node->isArchived = true;
    } else if (!node) {
        addThread(thread, accountId);
        return;
    } else {
        node->isArchived = false;
    }

    if (thread.name.hasValue())
        node->name = thread.name.get();
    if (thread.lastMessageId.hasValue())
        node->lastMessageId = thread.effectiveLastMessageId();

    changed.publish(node);

    resortThread(node);
}

void ChannelTreeModel::removeThread(Snowflake threadId, Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *node = findChannelTreeNode(threadId, accNode);
    if (!node || node->type != ChannelNode::Type::Thread)
        return;

    ChannelNode *parent = node->parent;
    if (!parent || parent->type != ChannelNode::Type::Channel)
        return;

    if (!removeChildRow(parent, node))
        return;

    updateNodeAggregates(parent);
    changed.publish(parent);
}

bool ChannelTreeModel::removeChildRow(ChannelNode *parent, ChannelNode *node)
{
    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i].get() != node)
            continue;
        int row = static_cast<int>(i);
        parent->children.erase(parent->children.begin() + row);
        reset.publish();
        return true;
    }
    return false;
}

void ChannelTreeModel::resortThread(ChannelNode *node)
{
    ChannelNode *parent = node->parent;
    if (!parent || parent->type != ChannelNode::Type::Channel)
        return;

    auto &kids = parent->children;
    int row = -1;
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i].get() == node) {
            row = static_cast<int>(i);
            break;
        }
    if (row == -1)
        return;

    int dest = static_cast<int>(kids.size());
    for (size_t i = 0; i < kids.size(); ++i) {
        if (static_cast<int>(i) == row)
            continue;
        if (kids[i]->lastMessageId < node->lastMessageId) {
            dest = static_cast<int>(i);
            break;
        }
    }
    if (dest == row || dest == row + 1)
        return;

    auto moved = std::move(kids[row]);
    kids.erase(kids.begin() + row);
    kids.insert(kids.begin() + (dest > row ? dest - 1 : dest), std::move(moved));
    reset.publish();
}

void ChannelTreeModel::syncThreads(Snowflake guildId, const std::vector<Snowflake> &parentIds,
                                   const std::vector<Discord::Channel> &threads, Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;
    ChannelNode *guildNode = findGuildNodeById(guildId, accNode);
    if (!guildNode)
        return;

    auto *instance = session->client(accountId);

    for (const auto &thread : threads)
        updateThread(thread, accountId);

    std::unordered_set<Snowflake> parentFilter(parentIds.begin(), parentIds.end());
    std::vector<Snowflake> toRemove;
    std::function<void(ChannelNode *)> collect = [&](ChannelNode *node) {
        for (const auto &child : node->children) {
            if (child->type == ChannelNode::Type::Thread) {
                if (node->type != ChannelNode::Type::Channel)
                    continue;
                bool inScope = parentFilter.empty() || parentFilter.contains(child->parentId);
                if (!inScope || child->isTemporary)
                    continue;
                bool keep = false;
                if (instance && instance->isThreadJoined(child->id)) {
                    auto ch = instance->getChannel(child->id);
                    keep = ch && instance->readState()->isThreadRelevant(*ch);
                }
                if (!keep)
                    toRemove.push_back(child->id);
            } else {
                collect(child.get());
            }
        }
    };
    collect(guildNode);

    for (Snowflake id : toRemove)
        removeThread(id, accountId);
}

void ChannelTreeModel::showTemporaryThread(const Discord::Channel &thread, Snowflake accountId)
{
    Snowflake threadId = thread.id.get();
    if (temporaryThreadId == threadId && temporaryThreadAccount == accountId)
        return;

    clearTemporaryThread();

    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *node = findChannelTreeNode(threadId, accNode);
    if (!node) {
        node = insertThreadNode(thread, accountId, true);
        if (!node)
            return;
    } else {
        node->isTemporary = true;
        changed.publish(node);
    }

    temporaryThreadId = threadId;
    temporaryThreadAccount = accountId;
}

void ChannelTreeModel::clearTemporaryThread(Snowflake exceptThreadId)
{
    if (!temporaryThreadId.isValid() || temporaryThreadId == exceptThreadId)
        return;

    Snowflake id = temporaryThreadId;
    Snowflake acc = temporaryThreadAccount;
    temporaryThreadId = Snowflake::Invalid;
    temporaryThreadAccount = Snowflake::Invalid;

    ChannelNode *accNode = accountNode(acc);
    if (!accNode)
        return;
    ChannelNode *node = findChannelTreeNode(id, accNode);
    if (node && node->type == ChannelNode::Type::Thread && node->isTemporary)
        removeThread(id, acc);
}

void ChannelTreeModel::promoteTemporaryThread(Snowflake threadId)
{
    if (temporaryThreadId == threadId) {
        temporaryThreadId = Snowflake::Invalid;
        temporaryThreadAccount = Snowflake::Invalid;
    }

    for (auto it = accountNodes.begin(); it != accountNodes.end(); ++it) {
        if (ChannelNode *node = findChannelTreeNode(threadId, it->second)) {
            node->isTemporary = false;
            return;
        }
    }
}

std::unique_ptr<ChannelNode> ChannelTreeModel::createFolderNode(const Proto::GuildFolder &folder)
{
    auto folderNode = std::make_unique<ChannelNode>();
    folderNode->type = ChannelNode::Type::Folder;
    folderNode->name = folder.name.value_or("Unnamed Folder");
    folderNode->folderName = folder.name;
    if (folder.color != Discord::BLURPLE)
        folderNode->folderColor = folder.color;
    folderNode->id = Core::Snowflake(folder.id.value());
    return folderNode;
}

ChannelNode *ChannelTreeModel::accountNode(Snowflake accountId) const
{
    auto it = accountNodes.find(accountId);
    return it == accountNodes.end() ? nullptr : it->second;
}

ChannelNode *ChannelTreeModel::findChannelTreeNode(Snowflake channelId)
{
    return findChannelTreeNode(channelId, root.get());
}

ChannelNode *ChannelTreeModel::findChannelTreeNode(Snowflake channelId, Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return nullptr;
    return findChannelTreeNode(channelId, accNode);
}

ChannelNode *ChannelTreeModel::findChannelTreeNode(Snowflake channelId, ChannelNode *searchRoot)
{
    if (!searchRoot)
        return nullptr;

    if (searchRoot->type != ChannelNode::Type::Server && searchRoot->id == channelId)
        return searchRoot;

    for (const auto &child : searchRoot->children) {
        if (ChannelNode *found = findChannelTreeNode(channelId, child.get()))
            return found;
    }

    return nullptr;
}

ChannelNode *ChannelTreeModel::findGuildNode(ChannelNode *node)
{
    while (node && node->type != ChannelNode::Type::Server)
        node = node->parent;
    return node;
}

ChannelNode *ChannelTreeModel::findCategoryNode(Snowflake categoryId, ChannelNode *guildNode)
{
    if (!guildNode || guildNode->type != ChannelNode::Type::Server)
        return nullptr;

    for (const auto &child : guildNode->children)
        if (child->type == ChannelNode::Type::Category && child->id == categoryId)
            return child.get();

    return nullptr;
}

ChannelNode *ChannelTreeModel::serverNode(Snowflake accountId, Snowflake guildId)
{
    return findGuildNodeById(guildId, accountNode(accountId));
}

ChannelNode *ChannelTreeModel::folderNode(Snowflake accountId, Snowflake folderId)
{
    return findFolderNodeById(accountNode(accountId), folderId);
}

ChannelNode *ChannelTreeModel::dmHeaderNode(Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return nullptr;
    for (const auto &child : accNode->children)
        if (child->type == ChannelNode::Type::DMHeader)
            return child.get();
    return nullptr;
}

ChannelNode *ChannelTreeModel::findGuildNodeById(Snowflake guildId, ChannelNode *accountNode)
{
    if (!accountNode || accountNode->type != ChannelNode::Type::Account)
        return nullptr;

    for (const auto &child : accountNode->children) {
        if (child->type == ChannelNode::Type::Server && child->id == guildId)
            return child.get();

        if (child->type == ChannelNode::Type::Folder) {
            for (const auto &folderChild : child->children)
                if (folderChild->type == ChannelNode::Type::Server && folderChild->id == guildId)
                    return folderChild.get();
        }
    }

    return nullptr;
}

ChannelNode *ChannelTreeModel::findFolderNodeById(ChannelNode *accountNode, Snowflake folderId)
{
    if (!accountNode)
        return nullptr;

    for (const auto &child : accountNode->children)
        if (child->type == ChannelNode::Type::Folder && child->id == folderId)
            return child.get();

    return nullptr;
}

void ChannelTreeModel::insertChildAt(ChannelNode *parent, int row, std::unique_ptr<ChannelNode> node)
{
    node->parent = parent;
    parent->children.insert(parent->children.begin() + row, std::move(node));
    reset.publish();
}

void ChannelTreeModel::addGuild(const Discord::GatewayGuild &guild, Snowflake accountId)
{
    if (!guild.properties.hasValue())
        return;

    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    auto *instance = session->client(accountId);
    Snowflake guildId = guild.properties->id.get();

    auto guildNode = createGuildNode(guild, instance);
    ChannelNode *guildPtr = guildNode.get();

    // compute read state on the detached subtree so the views see final values on insert
    if (instance)
        refreshReadStates(guildPtr, instance);

    // handle a (potentially) unavailable guild coming back
    if (ChannelNode *existing = findGuildNodeById(guildId, accNode)) {

        if (!existing->unavailable) {
            existing->name = guildPtr->name;
            existing->TEMP_iconHash = guildPtr->TEMP_iconHash;
            if (guildPtr->rulesChannelId.isValid())
                existing->rulesChannelId = guildPtr->rulesChannelId;
            changed.publish(existing);
            return;
        }

        ChannelNode *parentNode = existing->parent;
        if (!parentNode)
            return;
        auto existingIt = std::find_if(parentNode->children.begin(), parentNode->children.end(),
                                       [existing](const auto &child) { return child.get() == existing; });
        if (existingIt == parentNode->children.end())
            return;
        int existingRow = static_cast<int>(existingIt - parentNode->children.begin());

        if (!removeChildRow(parentNode, existing))
            return;

        insertChildAt(parentNode, existingRow, std::move(guildNode));

        updateNodeAggregates(parentNode);
        return;
    }

    placeGuildNode(accNode, guildId, std::move(guildNode), instance);
}

void ChannelTreeModel::updateGuild(const Discord::Guild &guild, Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *node = findGuildNodeById(guild.id.get(), accNode);
    if (!node)
        return;

    const Snowflake previousRulesChannel = node->rulesChannelId;
    node->name = guild.name;
    node->TEMP_iconHash = guild.icon;
    node->ownerId = guild.ownerId;
    node->rulesChannelId = guild.rulesChannelId.hasValue() ? guild.rulesChannelId.get() : Snowflake();

    changed.publish(node);

    if (previousRulesChannel == node->rulesChannelId)
        return;
    for (Snowflake channelId : { previousRulesChannel, node->rulesChannelId }) {
        if (auto *channelNode = findChannelTreeNode(channelId, node))
            changed.publish(channelNode);
    }
}

void ChannelTreeModel::removeGuild(Snowflake accountId, Snowflake guildId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *guildNode = findGuildNodeById(guildId, accNode);
    if (!guildNode)
        return;

    ChannelNode *parent = guildNode->parent;
    if (!parent)
        return;

    if (removeChildRow(parent, guildNode))
        updateNodeAggregates(parent);
}

void ChannelTreeModel::placeGuildNode(ChannelNode *accNode, Snowflake guildId,
                                      std::unique_ptr<ChannelNode> guildNode,
                                      Core::ClientInstance *instance)
{
    auto topRow = [accNode] {
        int r = 0;
        while (r < static_cast<int>(accNode->children.size()) &&
               accNode->children[r]->type == ChannelNode::Type::DMHeader)
            r++;
        return r;
    };

    const Proto::GuildFolder *folder = nullptr;
    if (instance) {
        const auto &settings = instance->discord()->getSettings();
        if (settings.guildFolders.has_value())
            for (const auto &f : settings.guildFolders->folders)
                if (f.id.has_value() && std::find(f.guildIds.begin(), f.guildIds.end(), guildId) != f.guildIds.end()) {
                    folder = &f;
                    break;
                }
    }

    ChannelNode *parent = accNode;
    int row = topRow();

    if (folder) {
        if (ChannelNode *folderNode = findFolderNodeById(accNode, Core::Snowflake(folder->id.value()))) {
            parent = folderNode;
            row = 0;
            for (Snowflake gid : folder->guildIds) {
                if (gid == guildId)
                    break;
                for (const auto &c : folderNode->children)
                    if (c->type == ChannelNode::Type::Server && c->id == gid) {
                        row++;
                        break;
                    }
            }
        } else {
            auto newFolder = createFolderNode(*folder);
            newFolder->addChild(std::move(guildNode));
            aggregateChildren(newFolder.get());
            guildNode = std::move(newFolder);
        }
    }

    insertChildAt(parent, row, std::move(guildNode));

    if (parent->type == ChannelNode::Type::Folder)
        updateNodeAggregates(parent);
}

void ChannelTreeModel::addChannel(const Discord::ChannelCreate &event, Snowflake accountId)
{
    if (!event.channel.hasValue())
        return;

    const auto &channel = event.channel.get();

    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    if (channel.type == Discord::ChannelType::DM || channel.type == Discord::ChannelType::GROUP_DM) {
        ChannelNode *dmHeader = nullptr;
        for (const auto &child : accNode->children) {
            if (child->type == ChannelNode::Type::DMHeader) {
                dmHeader = child.get();
                break;
            }
        }

        if (!dmHeader)
            return;

        if (findChannelTreeNode(channel.id, dmHeader))
            return;

        auto *instance = session->client(accountId);
        if (!instance)
            return;

        auto dmNode = std::make_unique<ChannelNode>();
        dmNode->id = channel.id;
        dmNode->type = ChannelNode::Type::DMChannel;
        updatePrivateChannelNode(*dmNode, channel, instance->users());

        insertChildAt(dmHeader, 0, std::move(dmNode));

        return;
    }

    auto nodeType = nodeTypeForChannel(channel.type);
    if (!nodeType)
        return;

    if (!channel.guildId.hasValue())
        return;

    Snowflake guildId = channel.guildId.get();
    ChannelNode *guildNode = findGuildNodeById(guildId, accNode);
    if (!guildNode)
        return;

    if (findChannelTreeNode(channel.id, guildNode))
        return;

    auto node = std::make_unique<ChannelNode>();
    node->id = channel.id;
    node->name = channel.name;
    node->position = channel.position;
    node->parentId = channel.parentId.hasValue() ? channel.parentId.get() : Core::Snowflake();
    if (channel.lastMessageId.hasValue())
        node->lastMessageId = channel.lastMessageId.get();

    node->type = *nodeType;
    if (*nodeType == ChannelNode::Type::Category) {
        insertChildAt(guildNode, static_cast<int>(guildNode->children.size()), std::move(node));
    } else {
        node->isPrivate = isChannelPrivate(channel, guildId);
        node->isAnnouncement = channel.type == Discord::ChannelType::GUILD_NEWS;
        if (*nodeType == ChannelNode::Type::VoiceChannel && channel.userLimit.hasValue())
            node->userLimit = channel.userLimit.get();

        ChannelNode *parentNode = nullptr;
        if (node->parentId.isValid())
            parentNode = findCategoryNode(node->parentId, guildNode);
        if (!parentNode)
            parentNode = guildNode;

        insertChildAt(parentNode, static_cast<int>(parentNode->children.size()), std::move(node));

        // notify proxy to re-check category visibility
        if (parentNode->type == ChannelNode::Type::Category)
            changed.publish(parentNode);
    }
}

void ChannelTreeModel::updateChannel(const Discord::ChannelUpdate &update, Snowflake accountId)
{
    const auto &channel = update.channel.get();

    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channel.id, accNode);
    if (!channelNode)
        return;

    auto *instance = session->client(accountId);
    if (!instance)
        return;

    if (channelNode->type == ChannelNode::Type::DMChannel) {
        updatePrivateChannelNode(*channelNode, channel, instance->users());
        changed.publish(channelNode);
        return;
    }

    Core::Snowflake oldParentId = channelNode->parentId;
    Core::Snowflake newParentId =
            channel.parentId.hasValue() ? channel.parentId.get() : Core::Snowflake();

    bool parentChanged = oldParentId != newParentId;

    if (parentChanged) {
        ChannelNode *oldParent = channelNode->parent;
        ChannelNode *guildNode = findGuildNode(channelNode);

        if (!guildNode)
            return;

        ChannelNode *newParent = newParentId.isValid() ? findCategoryNode(newParentId, guildNode) : guildNode;
        if (!oldParent || !newParent)
            return;
        int oldRow = -1;
        for (size_t i = 0; i < oldParent->children.size(); ++i) {
            if (oldParent->children[i].get() == channelNode) {
                oldRow = i;
                break;
            }
        }

        if (oldRow == -1)
            return;

        auto node = std::move(oldParent->children[oldRow]);
        oldParent->children.erase(oldParent->children.begin() + oldRow);

        node->name = channel.name.get();
        node->position = channel.position.get();
        node->parentId = newParentId;
        node->isPrivate = isChannelPrivate(channel, guildNode->id);
        node->isAnnouncement = channel.type == Discord::ChannelType::GUILD_NEWS;
        if (node->type == ChannelNode::Type::VoiceChannel && channel.userLimit.hasValue())
            node->userLimit = channel.userLimit.get();

        node->parent = newParent;
        newParent->children.push_back(std::move(node));
        reset.publish();
        updateNodeAggregates(oldParent);
        if (newParent != oldParent)
            updateNodeAggregates(newParent);
        if (oldParent->type == ChannelNode::Type::Category)
            changed.publish(oldParent);
        if (newParent->type == ChannelNode::Type::Category)
            changed.publish(newParent);
    } else {
        channelNode->name = channel.name.get();
        channelNode->position = channel.position.get();
        channelNode->parentId = newParentId;

        ChannelNode *guildNode = findGuildNode(channelNode);
        if (guildNode)
            channelNode->isPrivate = isChannelPrivate(channel, guildNode->id);
        channelNode->isAnnouncement = channel.type == Discord::ChannelType::GUILD_NEWS;

        if (channelNode->type == ChannelNode::Type::VoiceChannel && channel.userLimit.hasValue())
            channelNode->userLimit = channel.userLimit.get();

        changed.publish(channelNode);
    }
}

void ChannelTreeModel::deleteChannel(const Discord::ChannelDelete &event, Snowflake accountId)
{
    if (!event.id.hasValue())
        return;

    Core::Snowflake channelId = event.id.get();

    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode)
        return;

    ChannelNode *parent = channelNode->parent;
    if (!parent)
        return;

    if (!removeChildRow(parent, channelNode))
        return;

    updateNodeAggregates(parent);

    if (parent->type == ChannelNode::Type::Category)
        changed.publish(parent);
}

void ChannelTreeModel::invalidateGuildData(Snowflake guildId)
{
    for (const auto &[id, account] : accountNodes)
        if (ChannelNode *guildNode = findGuildNodeById(guildId, account))
            emitDataChangedRecursive(guildNode);
}

Core::ChannelReadState ChannelTreeModel::shownReadState(const ChannelNode *node)
{
    return { node->isUnread, node->mentionCount, node->isMuted, node->countsForGuildUnread };
}

bool ChannelTreeModel::notifyIfReadStateChanged(ChannelNode *node, const Core::ChannelReadState &before)
{
    if (shownReadState(node) == before)
        return false;

    changed.publish(node);
    return true;
}

void ChannelTreeModel::updateReadState(Snowflake channelId, Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode)
        return;

    auto *instance = session->client(accountId);
    if (!instance)
        return;

    Core::ChannelReadState before = shownReadState(channelNode);
    setSelfReadState(channelNode, computeNodeReadState(channelNode, instance));
    if (notifyIfReadStateChanged(channelNode, before))
        updateNodeAggregates(channelNode->parent);
}

void ChannelTreeModel::updatePresence(Snowflake accountId, const std::vector<Snowflake> &userIds)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode || userIds.empty())
        return;

    const std::unordered_set<Snowflake> changedUsers(userIds.cbegin(), userIds.cend());

    std::function<void(ChannelNode *)> visit = [&](ChannelNode *node) {
        if ((node->type == ChannelNode::Type::DMChannel ||
             node->type == ChannelNode::Type::VoiceParticipant) &&
            node->dmRecipientId.isValid() && changedUsers.contains(node->dmRecipientId)) {
            changed.publish(node);
        }

        for (const auto &child : node->children)
            visit(child.get());
    };

    visit(accNode);
}

void ChannelTreeModel::updateForumBadge(Snowflake forumId, Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *node = findChannelTreeNode(forumId, accNode);
    if (!node || node->type != ChannelNode::Type::Forum)
        return;

    auto *instance = session->client(accountId);
    if (!instance)
        return;

    Core::ForumBadge badge = instance->forums()->badge(forumId);
    if (badge.count == node->forumBadgeCount && badge.isNew == node->forumBadgeIsNew)
        return;

    node->forumBadgeCount = badge.count;
    node->forumBadgeIsNew = badge.isNew;
    changed.publish(node);
}

void ChannelTreeModel::updateForumThreads(Snowflake forumId, Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *forumNode = findChannelTreeNode(forumId, accNode);
    if (!forumNode || forumNode->type != ChannelNode::Type::Forum)
        return;

    auto *instance = session->client(accountId);
    if (!instance)
        return;

    const auto posts = instance->forums()->joinedPosts(forumId);

    if (posts.size() == forumNode->children.size()) {
        bool same = true;
        for (size_t i = 0; i < posts.size() && same; i++)
            same = forumNode->children[i]->id == posts[i].id.get();
        if (same) {
            for (size_t i = 0; i < posts.size(); i++) {
                ChannelNode *child = forumNode->children[i].get();
                auto lastMessageId = posts[i].effectiveLastMessageId();
                const std::string name = posts[i].name.hasValue() ? posts[i].name.get() : std::string();
                if (child->name == name && child->lastMessageId == lastMessageId)
                    continue;
                child->name = name;
                child->lastMessageId = lastMessageId;
                changed.publish(child);
            }
            return;
        }
    }

    forumNode->children.clear();
    for (const auto &post : posts) {
            auto node = std::make_unique<ChannelNode>();
            node->id = post.id.get();
            node->name = post.name.hasValue() ? post.name.get() : std::string();
            node->type = ChannelNode::Type::Thread;
            node->parentId = forumId;
            node->lastMessageId = post.effectiveLastMessageId();
            ChannelNode *added = forumNode->addChild(std::move(node));
            setSelfReadState(added, computeNodeReadState(added, instance));
        }
    reset.publish();

    updateNodeAggregates(forumNode);
}

void ChannelTreeModel::updateGuildSettings(Snowflake guildId, Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    auto *instance = session->client(accountId);
    if (!instance)
        return;

    ChannelNode *targetNode = nullptr;
    if (guildId == Snowflake(0)) {
        for (const auto &child : accNode->children) {
            if (child->type == ChannelNode::Type::DMHeader) {
                targetNode = child.get();
                break;
            }
        }
    } else {
        targetNode = findGuildNodeById(guildId, accNode);
    }

    if (!targetNode)
        return;

    refreshReadStates(targetNode, instance);
    emitDataChangedRecursive(targetNode);
    updateNodeAggregates(targetNode->parent);
}

Core::ChannelReadState ChannelTreeModel::computeNodeReadState(ChannelNode *node, Core::ClientInstance *instance)
{
    auto *readState = instance->readState();
    ChannelNode *guildNode = findGuildNode(node);
    Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;

    Core::ChannelReadState state;
    switch (node->type) {
    case ChannelNode::Type::Server:
        state.isMuted = readState->isGuildMuted(node->id);
        break;
    case ChannelNode::Type::Category:
        state.isMuted = readState->isChannelMuted(node->id);
        break;
    case ChannelNode::Type::Channel:
    case ChannelNode::Type::Forum:
        return readState->computeChannelReadState(node->id, guildId, node->parentId);
    case ChannelNode::Type::VoiceChannel:
        state = readState->computeChannelReadState(node->id, guildId, node->parentId);

        if (instance->voiceChannelId() != node->id)
            state.isUnread = false;
        return state;
    case ChannelNode::Type::DMChannel:
        return readState->computeDMReadState(node->id);
    case ChannelNode::Type::Thread: {
        Snowflake categoryId = node->parent ? node->parent->parentId : Snowflake::Invalid;
        if (node->parent && node->parent->type == ChannelNode::Type::Forum)
            return readState->computeForumPostReadState(node->id, guildId, node->parent->id, categoryId);
        return readState->computeThreadReadState(node->id, guildId, node->parentId, categoryId, instance->isThreadJoined(node->id));
    }
    default:
        break;
    }
    return state;
}

void ChannelTreeModel::setSelfReadState(ChannelNode *node, const Core::ChannelReadState &state)
{
    node->self = state;
    node->isMuted = state.isMuted;
    aggregateChildren(node);
}

void ChannelTreeModel::aggregateChildren(ChannelNode *node)
{
    node->isUnread = node->self.isUnread;
    node->mentionCount = node->self.mentionCount;
    node->countsForGuildUnread = node->self.countsForGuildUnread;
    for (const auto &child : node->children) {
        if (child->isUnread && !child->isMuted)
            node->isUnread = true;
        if (child->countsForGuildUnread)
            node->countsForGuildUnread = true;
        node->mentionCount += child->mentionCount;
    }

    if (node->type == ChannelNode::Type::Server || node->type == ChannelNode::Type::Folder ||
        node->type == ChannelNode::Type::DMHeader)
        node->isUnread = node->countsForGuildUnread;
}

void ChannelTreeModel::refreshReadStates(ChannelNode *node, Core::ClientInstance *instance)
{
    for (const auto &child : node->children)
        refreshReadStates(child.get(), instance);
    setSelfReadState(node, computeNodeReadState(node, instance));
}

void ChannelTreeModel::updateNodeAggregates(ChannelNode *node)
{
    for (; node && node != root.get(); node = node->parent) {
        Core::ChannelReadState before = shownReadState(node);
        aggregateChildren(node);
        if (!notifyIfReadStateChanged(node, before))
            return;
    }
    publishTotalMentionCount();
}

void ChannelTreeModel::publishTotalMentionCount()
{
    int total = 0;
    for (const auto &account : root->children)
        total += account->mentionCount;

    if (total == publishedMentionCount)
        return;

    int previous = std::exchange(publishedMentionCount, total);
    totalMentionCountChanged.publish(total, previous);
}

void ChannelTreeModel::emitDataChangedRecursive(ChannelNode *node)
{
    if (!node)
        return;
    changed.publish(node);
    for (const auto &child : node->children)
        emitDataChangedRecursive(child.get());
}

void ChannelTreeModel::toggleCollapsed(ChannelNode *node)
{
    if (node)
        setCollapsed(node, !node->collapsed);
}

void ChannelTreeModel::setCollapsed(ChannelNode *node, bool collapsed)
{
    if (!node || node->collapsed == collapsed)
        return;
    node->collapsed = collapsed;
    changed.publish(node);
}

void ChannelTreeModel::updateChannelLastMessageId(Snowflake channelId, Snowflake messageId,
                                                  Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode)
        return;

    if (channelNode->lastMessageId.isValid() && messageId <= channelNode->lastMessageId)
        return;

    channelNode->lastMessageId = messageId;
    updateReadState(channelId, accountId);
}

void ChannelTreeModel::setAccountVoiceChannel(Snowflake accountId, Snowflake channelId)
{
    Snowflake previous = Snowflake::Invalid;
    if (auto it = accountVoiceChannels.find(accountId); it != accountVoiceChannels.end())
        previous = it->second;
    if (previous == channelId)
        return;

    if (channelId.isValid())
        accountVoiceChannels[accountId] = channelId;
    else
        accountVoiceChannels.erase(accountId);

    if (previous.isValid())
        updateReadState(previous, accountId);
    if (channelId.isValid())
        updateReadState(channelId, accountId);
}

void ChannelTreeModel::updateVoiceCount(Snowflake channelId, int count, Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode || channelNode->type != ChannelNode::Type::VoiceChannel)
        return;

    if (channelNode->voiceParticipantCount == count)
        return;

    channelNode->voiceParticipantCount = count;

    changed.publish(channelNode);
}

void ChannelTreeModel::updateVoiceParticipant(Snowflake channelId, Snowflake userId, bool joined,
                                              Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode || channelNode->type != ChannelNode::Type::VoiceChannel)
        return;

    int existingRow = -1;
    for (size_t i = 0; i < channelNode->children.size(); ++i) {
        if (channelNode->children[i]->type == ChannelNode::Type::VoiceParticipant &&
            channelNode->children[i]->id == userId) {
            existingRow = static_cast<int>(i);
            break;
        }
    }

    if (joined) {
        if (existingRow != -1)
            return;

        auto *instance = session->client(accountId);
        if (!instance)
            return;

        ChannelNode *guildNode = findGuildNode(channelNode);
        Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;

        std::string displayName;
        std::string avatarUrl = Discord::Cdn::defaultUserAvatar(userId, VoiceParticipantAvatarPx);
        auto user = instance->users()->getUser(userId);
        if (user) {
            displayName = instance->users()->getDisplayName(userId, guildId);
            avatarUrl = instance->users()->getAvatarUrl(*user, guildId, VoiceParticipantAvatarPx);
        }
        if (displayName.empty())
            displayName = userId.toString();

        int insertRow = 0;
        for (size_t i = 0; i < channelNode->children.size(); ++i) {
            const auto &child = channelNode->children[i];
            if (child->type != ChannelNode::Type::VoiceParticipant)
                continue;
            if (Platform::unicodeLower(child->name) < Platform::unicodeLower(displayName))
                insertRow = static_cast<int>(i) + 1;
            else
                break;
        }

        auto participantNode = std::make_unique<ChannelNode>();
        participantNode->id = userId;
        participantNode->parentId = channelId;
        participantNode->type = ChannelNode::Type::VoiceParticipant;
        participantNode->name = displayName;
        participantNode->dmRecipientId = userId;
        participantNode->voiceParticipantAvatarUrl = avatarUrl;
        participantNode->parent = channelNode;

        insertChildAt(channelNode, insertRow, std::move(participantNode));
    } else {
        if (existingRow == -1)
            return;
        removeChildRow(channelNode, channelNode->children[existingRow].get());
    }
}

void ChannelTreeModel::updateVoiceParticipantState(Snowflake channelId, Snowflake userId,
                                                   Snowflake accountId)
{
    ChannelNode *accNode = accountNode(accountId);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode || channelNode->type != ChannelNode::Type::VoiceChannel)
        return;

    for (auto &child : channelNode->children) {
        if (child->type == ChannelNode::Type::VoiceParticipant && child->id == userId) {
            changed.publish(child.get());
            return;
        }
    }
}

void ChannelTreeModel::collectMarkableChannelIds(ChannelNode *node, std::vector<Snowflake> &out)
{
    if (node->opensChat())
        out.push_back(node->id);
    for (auto &child : node->children)
        collectMarkableChannelIds(child.get(), out);
}

std::vector<Snowflake> ChannelTreeModel::getMarkableChannelIds(ChannelNode *node)
{
    std::vector<Snowflake> result;
    if (node)
        collectMarkableChannelIds(node, result);
    return result;
}

} // namespace UI
} // namespace Styx
