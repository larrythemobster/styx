#include "ChannelFilterProxyModel.hpp"
#include "ChannelTreeModel.hpp"
#include "ChannelNode.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/PermissionManager.hpp"
#include "Core/ReadStateManager.hpp"
#include "Discord/Enums.hpp"
#include <algorithm>

namespace Styx::UI {
ChannelFilterProxyModel::ChannelFilterProxyModel(Core::Session *session) : session(session) {}

void ChannelFilterProxyModel::setSourceModel(ChannelTreeModel *sourceModel)
{
    channelModel = sourceModel;
    if (sourceModel) {
        track(sourceModel->changed.connect([this, sourceModel](ChannelNode *) { if (channelModel == sourceModel) changed.publish(); }));
        track(sourceModel->reset.connect([this, sourceModel] { if (channelModel == sourceModel) changed.publish(); }));
    }
    changed.publish();
}

std::vector<ChannelNode *> ChannelFilterProxyModel::children(ChannelNode *parent) const
{
    std::vector<ChannelNode *> result;
    if (!parent) parent = channelModel ? channelModel->rootNode() : nullptr;
    if (!parent) return result;
    for (const auto &child : parent->children)
        if (accepts(child.get())) result.push_back(child.get());
    auto rank = [](ChannelNode::Type type) {
        if (type == ChannelNode::Type::Channel || type == ChannelNode::Type::Forum) return 0;
        if (type == ChannelNode::Type::VoiceChannel) return 1;
        if (type == ChannelNode::Type::Category) return 2;
        return -1;
    };
    for (auto first = result.begin(); first != result.end();) {
        const bool guild = rank((*first)->type) >= 0;
        const bool dm = (*first)->type == ChannelNode::Type::DMChannel;
        auto last = first + 1;
        while (last != result.end() && ((guild && rank((*last)->type) >= 0) ||
                                        (dm && (*last)->type == ChannelNode::Type::DMChannel)))
            ++last;
        if (guild) {
            std::stable_sort(first, last, [&](const ChannelNode *a, const ChannelNode *b) {
                int ar = rank(a->type), br = rank(b->type);
                return ar == br ? a->position < b->position : ar < br;
            });
        } else if (dm) {
            std::stable_sort(first, last, [](const ChannelNode *a, const ChannelNode *b) {
                return a->lastMessageId > b->lastMessageId;
            });
        }
        first = last;
    }
    return result;
}

bool ChannelFilterProxyModel::selected(ChannelNode *node) const
{
    return node && node->id == selectedChannelId && getUserIdForNode(node) == selectedAccountId;
}
void ChannelFilterProxyModel::setSelectedChannel(Core::Snowflake channelId, Core::Snowflake accountId)
{
    if (selectedChannelId == channelId && selectedAccountId == accountId) return;
    selectedChannelId = channelId;
    selectedAccountId = accountId;
    invalidateFilter();
}
void ChannelFilterProxyModel::invalidateFilter() { changed.publish(); }

bool ChannelFilterProxyModel::accepts(ChannelNode *node) const
{
    if (!node || !channelModel) return true;
    auto type = node->type;
    if (type == ChannelNode::Type::VoiceParticipant || type == ChannelNode::Type::Thread) return true;
    auto userId = getUserIdForNode(node);
    auto *instance = userId.isValid() ? session->client(userId) : nullptr;
    if (!instance) return true;
    bool channel = type == ChannelNode::Type::Channel || type == ChannelNode::Type::VoiceChannel || type == ChannelNode::Type::Forum;
    if (channel || type == ChannelNode::Type::Category) {
        auto *guild = node;
        while (guild && guild->type != ChannelNode::Type::Server) guild = guild->parent;
        if (guild) {
            auto *state = instance->readState();
            const auto *preferences = state->guildSettings(guild->id);
            bool keep = node->mentionCount > 0 || node->id == instance->voiceChannelId();
            if (selectedAccountId == userId) {
                for (auto *selectedNode = channelModel->findChannelTreeNode(selectedChannelId, userId);
                     selectedNode && selectedNode->type != ChannelNode::Type::Server; selectedNode = selectedNode->parent) {
                    if (selectedNode == node) { keep = true; break; }
                }
            }
            if (!keep && preferences && preferences->hideMutedChannels.valueOr(false)) {
                for (auto *parent = node; parent && parent != guild; parent = parent->parent)
                    if (state->isChannelMuted(parent->id)) return false;
            }
            if (!keep && state->isOptInGuild(guild->id) && !state->isChannelOptedIn(node->id) &&
                !(node->parent && node->parent->type == ChannelNode::Type::Category && state->isChannelOptedIn(node->parent->id))) {
                if (type != ChannelNode::Type::Category) return false;
                return std::any_of(node->children.begin(), node->children.end(),
                                   [this](const auto &child) { return accepts(child.get()); });
            }
        }
    }
    if (channel && node->parent && node->parent->type == ChannelNode::Type::Category && node->parent->collapsed) {
        if (selected(node)) return true;
        if (type == ChannelNode::Type::VoiceChannel) {
            if (node->id != instance->voiceChannelId() && node->mentionCount == 0) return false;
        } else if (!node->isUnread || node->isMuted || node->parent->isMuted) return false;
    }
    if (channel) return hasChannelViewPermission(node);
    if (type == ChannelNode::Type::Category) {
        auto *permissions = instance->permissions();
        if (!permissions || permissions->hasChannelPermission(userId, node->id, Discord::Permission::VIEW_CHANNEL | Discord::Permission::MANAGE_CHANNELS)) return true;
        for (const auto &child : node->children)
            if (hasChannelViewPermission(child.get())) return true;
        return false;
    }
    return true;
}
bool ChannelFilterProxyModel::hasChannelViewPermission(ChannelNode *node) const
{
    auto userId = getUserIdForNode(node);
    auto *instance = userId.isValid() ? session->client(userId) : nullptr;
    auto *permissions = instance ? instance->permissions() : nullptr;
    return !permissions || permissions->hasChannelPermission(userId, node->id, Discord::Permission::VIEW_CHANNEL);
}
Core::Snowflake ChannelFilterProxyModel::getUserIdForNode(ChannelNode *node)
{
    auto *account = ChannelTreeModel::getAccountNodeFor(node);
    return account ? account->id : Core::Snowflake::Invalid;
}
}
