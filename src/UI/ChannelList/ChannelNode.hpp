#pragma once

#include <string>
#include <optional>
#include <cstdint>

#include <memory>
#include <vector>

#include "Core/ChannelReadState.hpp"
#include "Core/Snowflake.hpp"

namespace Styx {
namespace UI {

struct ChannelNode
{
    enum class Type {
        Root,
        Account,
        DMHeader,
        Folder,
        Server,
        Category,
        Channel,
        Forum,
        Thread,
        VoiceChannel,
        VoiceParticipant,
        DMChannel,
    };

    Core::Snowflake id;
    std::string name;
    Type type;
    int position = 0;
    Core::ChannelReadState self; // before descendants
    bool isUnread = false;
    int mentionCount = 0;
    bool isMuted = false;
    bool countsForGuildUnread = false;
    bool collapsed = false;
    bool unavailable = false;
    bool isArchived = false;
    bool isTemporary = false;
    bool isAnnouncement = false;
    Core::Snowflake parentId;
    Core::Snowflake rulesChannelId;
    Core::Snowflake ownerId;
    std::string TEMP_iconHash;
    std::optional<std::string> folderName;
    std::optional<uint64_t> folderColor;
    std::vector<Core::Snowflake> recipientIds;
    std::string dmAvatarHash;
    Core::Snowflake dmRecipientId;
    std::string voiceParticipantAvatarUrl;
    Core::Snowflake lastMessageId;
    bool isPrivate = false;
    int voiceParticipantCount = 0;
    int userLimit = 0;
    int forumBadgeCount = 0;
    bool forumBadgeIsNew = false;

    std::vector<std::unique_ptr<ChannelNode>> children;
    ChannelNode *parent = nullptr;
    mutable std::uint64_t avatarTarget = 0;

    [[nodiscard]] bool opensChat() const
    {
        return type == Type::Channel ||
               type == Type::VoiceChannel ||
               type == Type::DMChannel ||
               type == Type::Forum ||
               type == Type::Thread;
    }

    ChannelNode *addChild(std::unique_ptr<ChannelNode> node)
    {
        node->parent = this;
        children.push_back(std::move(node));
        return children.back().get();
    }
};

} // namespace UI
} // namespace Styx
