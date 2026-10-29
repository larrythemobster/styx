#include "ChannelListController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"
#include "Core/Audio/VoiceManager.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Presence/ActivityFormat.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Platform/Native.hpp"
#include "Platform/Settings.hpp"
#include "UI/ChannelList/ChannelFilterProxyModel.hpp"
#include "UI/ChannelList/ChannelNode.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"

#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

#include <charconv>
#include <chrono>
#include <string_view>

namespace Styx::UI::Rml {

using Core::Snowflake;

namespace {
Snowflake snowflake(const std::string &text)
{
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? Snowflake(value) : Snowflake::Invalid;
}

std::string categoryKey(Snowflake account, Snowflake category)
{
    return "category_collapsed/" + account.toString() + "/" + category.toString();
}

std::string trimmed(std::string_view text)
{
    const auto start = text.find_first_not_of(" \t\r\n\f\v");
    if (start == std::string_view::npos) return {};
    return std::string(text.substr(start, text.find_last_not_of(" \t\r\n\f\v") - start + 1));
}

void appendMention(std::string &rml, int mentions)
{
    if (mentions > 0)
        rml += "<div class=\"channel-mention-badge\">" + (mentions > 99 ? std::string("99+") : std::to_string(mentions)) + "</div>";
}
}

ChannelListController::ChannelListController(Shell &shell, ::Rml::Element *root)
    : Controller(shell, root)
{
    if (auto *doc = shell.document()) {
        sidebarHeader = doc->GetElementById("sidebar-header");
        channelList = doc->GetElementById("channel-list");
    }
    if (sidebarHeader) {
        listen("click", [this](::Rml::Event &event) {
            if (auto *act = closest(event.GetTargetElement(), "data-action")) {
                if (attribute(act, "data-action") == "clear-dm-search") {
                    dmFilterText.clear();
                    if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(sidebarHeader->GetElementById("sidebar-search-input")))
                        input->SetValue("");
                    scheduleRebuild();
                    return;
                }
            }
            if (!this->shell.location().isHome())
                this->shell.showContextMenuForGuild(this->shell.location().accountId, this->shell.location().guildId);
        }, sidebarHeader);
        auto search = [this](::Rml::Event &event) {
            if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(event.GetTargetElement())) {
                if (input->GetId() == "sidebar-search-input" && input->GetValue() != dmFilterText) {
                    dmFilterText = input->GetValue();
                    scheduleRebuild();
                }
            }
        };
        listen("change", search, sidebarHeader);
        listen("keyup", search, sidebarHeader, true);
    }
    if (channelList) {
        listen("click", [this](::Rml::Event &event) {
            auto *target = event.GetTargetElement();
            if (auto *act = closest(target, "data-action")) {
                if (attribute(act, "data-action") == "friends") {
                    this->shell.openFriends(this->shell.location().accountId);
                    return;
                }
            }
            if (auto *cat = closest(target, "data-category-id")) {
                auto id = snowflake(attribute(cat, "data-category-id"));
                if (auto *tree = this->shell.channelTree()) {
                    if (auto *node = tree->findChannelTreeNode(id, this->shell.location().accountId)) {
                        bool collapsed = !node->collapsed;
                        tree->setCollapsed(node, collapsed);
                        Platform::Settings::instance().setBool(categoryKey(this->shell.location().accountId, id), collapsed);
                    }
                }
                scheduleRebuild();
                return;
            }
            if (auto *chan = closest(target, "data-channel-id")) {
                auto id = snowflake(attribute(chan, "data-channel-id"));
                if (attribute(chan, "data-type") == "voice")
                    this->shell.joinVoice(this->shell.location().accountId, this->shell.location().guildId, id);
                this->shell.openChannel(this->shell.location().accountId, this->shell.location().guildId, id);
                return;
            }
            if (auto *dm = closest(target, "data-dm-id")) {
                this->shell.openChannel(this->shell.location().accountId, Snowflake::Invalid, snowflake(attribute(dm, "data-dm-id")));
                return;
            }
            if (auto *user = closest(target, "data-user-id"))
                this->shell.showUserProfile(snowflake(attribute(user, "data-user-id")), this->shell.location().guildId);
        }, channelList);
        listen("mousedown", [this](::Rml::Event &event) {
            if (event.GetParameter<int>("button", 0) != 1) return;
            auto *target = event.GetTargetElement();
            if (auto *chan = closest(target, "data-channel-id"))
                this->shell.showContextMenuForChannel(this->shell.location().accountId, this->shell.location().guildId, snowflake(attribute(chan, "data-channel-id")));
            else if (auto *dm = closest(target, "data-dm-id"))
                this->shell.showContextMenuForChannel(this->shell.location().accountId, Snowflake::Invalid, snowflake(attribute(dm, "data-dm-id")));
            else if (auto *user = closest(target, "data-user-id"))
                this->shell.showContextMenuForUser(snowflake(attribute(user, "data-user-id")), this->shell.location().guildId);
        }, channelList);
    }
    track(shell.locationChanged.connect([this](const Location &loc) {
        if (auto *filter = this->shell.channelFilter()) {
            filter->setSelectedChannel(loc.channelId, loc.accountId);
            filter->invalidateFilter();
        }
        scheduleRebuild();
    }));
    track(shell.instanceChanged.connect([this](Core::ClientInstance *instance) {
        connectInstanceVoice(instance);
        scheduleRebuild();
    }));
    track(shell.accountReady.connect([this](auto) { scheduleRebuild(); }));
    if (auto *tree = shell.channelTree()) {
        track(tree->reset.connect([this] { scheduleRebuild(); }));
        track(tree->changed.connect([this](auto *) { scheduleRebuild(); }));
    }
    if (auto *filter = shell.channelFilter())
        track(filter->changed.connect([this] { scheduleRebuild(); }));
    connectInstanceVoice(shell.currentInstance());
    renderSidebar();
}

void ChannelListController::scheduleRebuild()
{
    rebuildTimer.start(std::chrono::milliseconds(0), false, [this] { renderSidebar(); });
}

void ChannelListController::connectInstanceVoice(Core::ClientInstance *instance)
{
    if (lastInstance.get() == instance) return;
    instanceConnections.clear();
    lastInstance = instance;
    dmFilterText.clear();
    if (sidebarHeader) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(sidebarHeader->GetElementById("sidebar-search-input")))
            input->SetValue("");
    }
    if (!instance) return;
    instanceConnections.push_back(instance->presences()->presencesChanged.connect([this](auto) {
        if (shell.location().isHome()) scheduleRebuild();
    }));
#ifndef STYX_NO_VOICE
    if (auto *voice = instance->voice()) {
        instanceConnections.push_back(voice->channelVoiceMemberChanged.connect([this](auto, auto, auto) { scheduleRebuild(); }));
        instanceConnections.push_back(voice->participantSpeakingChanged.connect([this](auto, auto) { scheduleRebuild(); }));
        instanceConnections.push_back(voice->participantVoiceStateChanged.connect([this](auto, auto) { scheduleRebuild(); }));
        instanceConnections.push_back(voice->participantJoined.connect([this](auto) { scheduleRebuild(); }));
        instanceConnections.push_back(voice->participantLeft.connect([this](auto) { scheduleRebuild(); }));
        instanceConnections.push_back(voice->participantUpdated.connect([this](auto) { scheduleRebuild(); }));
    }
#endif
}

void ChannelListController::renderSidebar()
{
    if (!sidebarHeader || !channelList) return;
    if (shell.location().isHome()) renderHomeSidebar();
    else renderGuildSidebar();
    requestRender(200);
}

void ChannelListController::renderHomeSidebar()
{
    if (!sidebarHeader->GetElementById("sidebar-search-input")) {
        std::string header = "<div class=\"sidebar-search-box\"><img class=\"search-icon\" src=\"icon:search\"/><input type=\"text\" id=\"sidebar-search-input\" class=\"sidebar-search-input\" value=\"" + escape(dmFilterText) + "\"/><span id=\"sidebar-search-placeholder\" class=\"sidebar-search-placeholder\" style=\"" + (dmFilterText.empty() ? "" : "display: none;") + "\">Find or start a conversation</span><img id=\"sidebar-search-clear\" class=\"sidebar-search-clear\" src=\"icon:x\" data-action=\"clear-dm-search\" style=\"" + (dmFilterText.empty() ? "display: none;" : "") + "\"/></div>";
        sidebarHeader->SetInnerRML(header);
    } else if (auto *placeholder = sidebarHeader->GetElementById("sidebar-search-placeholder")) {
        placeholder->SetProperty("display", dmFilterText.empty() ? "inline" : "none");
    }
    if (auto *clear = sidebarHeader->GetElementById("sidebar-search-clear"))
        clear->SetProperty("display", dmFilterText.empty() ? "none" : "block");
    bool friendsSelected = shell.isFriendsOpen();
    std::string rml = "<div class=\"channel-row dm-friends-row" + std::string(friendsSelected ? " selected" : "") + "\" data-action=\"friends\"><img class=\"channel-icon\" src=\"icon:user-plus\"/><span class=\"channel-name\">Friends</span></div><div class=\"section-header\">Direct Messages</div>";
    auto *tree = shell.channelTree();
    auto *filter = shell.channelFilter();
    if (tree && filter) {
        auto *header = tree->dmHeaderNode(shell.location().accountId);
        if (header && filter->accepts(header)) {
            const auto query = Platform::unicodeLower(trimmed(dmFilterText));
            for (auto *node : filter->children(header)) {
                if (!query.empty() && Platform::unicodeLower(node->name).find(query) == std::string::npos) continue;
                std::string avatar;
                if (node->dmRecipientId.isValid())
                    avatar = shell.imageSource(Discord::Cdn::userAvatar(node->dmRecipientId, node->dmAvatarHash, 32), {32, 32});
                else if (!node->TEMP_iconHash.empty())
                    avatar = shell.imageSource(Discord::Cdn::channelIcon(node->id, node->TEMP_iconHash, 32), {32, 32});
                else
                    avatar = shell.imageSource(Discord::Cdn::defaultUserAvatar(node->dmRecipientId.isValid() ? node->dmRecipientId : node->id), {32, 32});
                auto badge = tree->presenceBadge(node);
                std::string status;
                switch (badge.status) {
                case Discord::StatusType::ONLINE: status = "status-online"; break;
                case Discord::StatusType::IDLE: status = "status-idle"; break;
                case Discord::StatusType::DND: status = "status-dnd"; break;
                default: status = "status-offline"; break;
                }
                if (badge.streaming) status = "status-streaming";
                std::string row = "dm-row";
                if (!friendsSelected && node->id == shell.location().channelId) row += " selected";
                if (node->isUnread) row += " unread";
                rml += "<div class=\"" + row + "\" data-dm-id=\"" + node->id.toString() + "\" data-recipient-id=\"" + node->dmRecipientId.toString() + "\"><div class=\"dm-avatar-wrap\"><div class=\"avatar-clip\"><img class=\"dm-avatar\" src=\"" + escape(avatar) + "\"/></div><div class=\"status-dot " + status + "\"></div></div>";
                std::string subtext;
                if (node->dmRecipientId.isValid()) {
                    if (auto *inst = shell.currentInstance()) {
                        auto acts = inst->presences()->activities(node->dmRecipientId);
                        auto sorted = Core::ActivityFormat::sortAndFilter(acts);
                        auto *custom = Core::ActivityFormat::custom(acts);
                        auto *primary = Core::ActivityFormat::primary(sorted);
                        if (custom && !custom->stateText().empty()) subtext = custom->stateText();
                        else if (primary) {
                            subtext = Core::ActivityFormat::cardHeading(*primary);
                            auto secondary = Core::ActivityFormat::secondaryText(*primary);
                            if (!secondary.empty()) subtext += ": " + secondary;
                        }
                    }
                } else if (!node->recipientIds.empty()) subtext = std::to_string(node->recipientIds.size() + 1) + " Members";
                rml += "<div class=\"dm-text\"><span class=\"dm-name\">" + escape(node->name) + "</span>";
                if (!subtext.empty()) rml += "<span class=\"dm-subtext\">" + escape(subtext) + "</span>";
                rml += "</div>";
                appendMention(rml, node->mentionCount);
                rml += "</div>";
            }
        }
    }
    channelList->SetInnerRML(rml);
}

void ChannelListController::renderGuildSidebar()
{
    auto *tree = shell.channelTree();
    auto *filter = shell.channelFilter();
    if (!tree || !filter) return;
    auto *guild = tree->serverNode(shell.location().accountId, shell.location().guildId);
    std::string name = guild && !guild->name.empty() ? guild->name : "Server";
    sidebarHeader->SetInnerRML("<div class=\"guild-header-content\"><span class=\"guild-header-title\">" + escape(name) + "</span><img class=\"guild-header-chevron\" src=\"icon:chevron-down\"/></div>");
    std::string rml;
    if (guild && filter->accepts(guild))
        for (auto *node : filter->children(guild)) renderChannelNode(node, rml);
    channelList->SetInnerRML(rml);
}

void ChannelListController::renderChannelNode(ChannelNode *node, std::string &rml)
{
    auto *tree = shell.channelTree();
    auto *filter = shell.channelFilter();
    if (!tree || !filter || !node) return;
    if (node->type == ChannelNode::Type::Category) {
        auto &settings = Platform::Settings::instance();
        auto key = categoryKey(shell.location().accountId, node->id);
        if (settings.contains(key) && node->collapsed != settings.getBool(key))
            tree->setCollapsed(node, settings.getBool(key));
        rml += "<div class=\"category-row\" data-category-id=\"" + node->id.toString() + "\"><img class=\"category-icon\" src=\"" + (node->collapsed ? "icon:chevron-right" : "icon:chevron-down") + "\"/><span class=\"category-name\">" + escape(Platform::unicodeUpper(node->name)) + "</span></div>";
        for (auto *child : filter->children(node)) renderChannelNode(child, rml);
        return;
    }
    if (node->type == ChannelNode::Type::VoiceParticipant) {
        bool speaking = false, streaming = false;
        bool muted = tree->voiceMuted(node), deafened = tree->voiceDeafened(node);
#ifndef STYX_NO_VOICE
        if (auto *inst = shell.currentInstance()) {
            if (auto *voice = inst->voice()) {
                if (auto *participant = voice->participant(node->id)) {
                    speaking = participant->speaking;
                    streaming = participant->streaming;
                    muted = muted || participant->selfMute || participant->serverMute;
                    deafened = deafened || participant->selfDeaf || participant->serverDeaf;
                }
            }
        }
#endif
        auto avatar = shell.imageSource(node->voiceParticipantAvatarUrl.empty() ? Discord::Cdn::defaultUserAvatar(node->id) : node->voiceParticipantAvatarUrl, {24, 24});
        rml += "<div class=\"voice-participant-row" + std::string(speaking ? " speaking" : "") + "\" data-user-id=\"" + node->id.toString() + "\"><img class=\"vp-avatar\" src=\"" + escape(avatar) + "\"/><span class=\"vp-name\">" + escape(node->name) + "</span>";
        if (streaming) rml += "<span class=\"vp-badge-live\">LIVE</span>";
        if (deafened) rml += "<img class=\"vp-status-icon\" src=\"icon:volume-x\"/>";
        else if (muted) rml += "<img class=\"vp-status-icon\" src=\"icon:mic-off\"/>";
        rml += "</div>";
        return;
    }
    bool forum = node->type == ChannelNode::Type::Forum;
    bool voice = node->type == ChannelNode::Type::VoiceChannel;
    bool thread = node->type == ChannelNode::Type::Thread;
    if (!forum && !voice && !thread && node->type != ChannelNode::Type::Channel) return;
    std::string row = thread ? "channel-row thread" : "channel-row";
    if (node->id == shell.location().channelId) row += " selected";
    if (node->isUnread) row += " unread";
    if (node->isMuted) row += " muted";
    const char *icon = voice ? "icon:volume-2" : thread ? "icon:message-circle" : forum ? "icon:messages-square" : node->isAnnouncement ? "icon:megaphone" : node->isPrivate ? "icon:lock" : "icon:hash";
    rml += "<div class=\"" + row + "\" data-channel-id=\"" + node->id.toString() + "\" data-type=\"" + (voice ? "voice" : forum ? "forum" : "text") + "\">";
    if (node->isUnread) rml += "<div class=\"channel-pill\"></div>";
    rml += "<img class=\"channel-icon\" src=\"" + std::string(icon) + "\"/><span class=\"channel-name\">" + escape(node->name) + "</span>";
    appendMention(rml, node->mentionCount);
    rml += "</div>";
    if (!thread) for (auto *child : filter->children(node)) renderChannelNode(child, rml);
}

} // namespace Styx::UI::Rml
