#include "MemberListController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/MemberListManager.hpp"
#include "Core/Presence/ActivityFormat.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"
#include "UI/MemberList/MemberListModel.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <string>

namespace Styx::UI::Rml {
using Core::Snowflake;

namespace {
Snowflake parseId(const std::string &text)
{
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? Snowflake(value) : Snowflake();
}

std::string statusClass(const Core::PresenceBadge &badge)
{
    if (badge.streaming) return "status-streaming";
    switch (badge.status) {
    case Discord::StatusType::ONLINE: return "status-online";
    case Discord::StatusType::IDLE: return "status-idle";
    case Discord::StatusType::DND: return "status-dnd";
    default: return "status-offline";
    }
}

std::string activityText(Core::PresenceManager *manager, Snowflake userId, Snowflake guildId = {})
{
    if (!manager) return {};
    const auto *activity = manager->primaryActivity(userId, guildId);
    return activity ? Core::ActivityFormat::secondaryText(*activity) : std::string();
}
}

MemberListController::MemberListController(Shell &shell, ::Rml::Element *root)
    : Controller(shell, root)
{
    setupConnections();
    listen("scroll", [this](::Rml::Event &) { onScroll(); });
    listen("click", [this](::Rml::Event &event) {
        auto *item = closest(event.GetTargetElement(), "data-user-id");
        if (!item) return;
        const auto userId = parseId(attribute(item, "data-user-id"));
        if (closest(event.GetTargetElement(), "data-profile-view")) {
            if (userId.isValid()) {
                if (auto *button = closest(event.GetTargetElement(), "data-profile-view")) button->Focus();
                this->shell.showUserProfile(userId, activeGuildId);
            }
            return;
        }
        if (userId.isValid()) {
            Platform::Rect anchor{item->GetAbsoluteLeft(), item->GetAbsoluteTop(), item->GetClientWidth(), item->GetClientHeight()};
            this->shell.showUserProfile(userId, activeGuildId, anchor);
        }
    });
    listen("keydown", [](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (target && target->HasAttribute("data-profile-view")
            && (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER || key == ::Rml::Input::KI_SPACE)) {
            event.StopPropagation();
            target->DispatchEvent("click", {});
        }
    });
    listen("contextmenu", [this](::Rml::Event &event) {
        auto *item = closest(event.GetTargetElement(), "data-user-id");
        if (!item) return;
        const auto userId = parseId(attribute(item, "data-user-id"));
        if (userId.isValid()) this->shell.showContextMenuForUser(userId, activeGuildId);
    });
    onInstanceChanged();
    onLocationChanged();
}

MemberListController::~MemberListController()
{
    updateDebounceTimer.stop();
    disconnectCurrentManager();
}

void MemberListController::scheduleUpdate()
{
    updateDebounceTimer.start(std::chrono::milliseconds(10), false, [this] { updateMemberList(); });
}

void MemberListController::setupConnections()
{
    track(shell.locationChanged.connect([this](const auto &) { onLocationChanged(); }));
    track(shell.instanceChanged.connect([this](auto *) { onInstanceChanged(); }));
    track(shell.memberListVisibleChanged.connect([this](bool visible) { if (visible) scheduleUpdate(); }));
    if (auto *model = shell.members()) {
        track(model->reset.connect([this] { scheduleUpdate(); }));
        track(model->rowsChanged.connect([this](const auto &) { scheduleUpdate(); }));
    }
}

void MemberListController::onInstanceChanged()
{
    disconnectCurrentManager();
    currentInstance = shell.currentInstance();
    currentMemberListManager = currentInstance ? currentInstance->memberList() : nullptr;
    currentPresenceManager = currentInstance ? currentInstance->presences() : nullptr;
    connectCurrentManager();
    lastSubStart = lastSubEnd = -1;
    scheduleUpdate();
}

void MemberListController::connectCurrentManager()
{
    if (currentMemberListManager) {
        managerConnections.push_back(currentMemberListManager->listReset.connect([this] { scheduleUpdate(); }));
        managerConnections.push_back(currentMemberListManager->itemsChanged.connect([this](const auto &) { scheduleUpdate(); }));
    }
    if (currentPresenceManager)
        managerConnections.push_back(currentPresenceManager->presencesChanged.connect([this](const auto &) { scheduleUpdate(); }));
    if (currentInstance) {
        managerConnections.push_back(currentInstance->channelUpdated.connect(
            [this](const Discord::ChannelUpdate &event) {
                if (event.channel->id == activeChannelId)
                    scheduleUpdate();
            }));
    }
}

void MemberListController::disconnectCurrentManager()
{
    managerConnections.clear();
}

void MemberListController::onLocationChanged()
{
    const auto &loc = shell.location();
    activeGuildId = loc.guildId;
    activeChannelId = loc.channelId;
    lastSubStart = lastSubEnd = -1;
    if (regionRoot) regionRoot->SetScrollTop(0);
    scheduleUpdate();
}

void MemberListController::onScroll()
{
    if (!regionRoot || !activeGuildId.isValid() || !currentMemberListManager) return;
    const int totalCount = currentMemberListManager->totalItemCount();
    if (totalCount <= 0) return;
    const int first = static_cast<int>(regionRoot->GetScrollTop() / RowHeight);
    const int count = static_cast<int>(std::ceil(regionRoot->GetClientHeight() / RowHeight)) + 1;
    const int start = std::max(0, first - 10);
    const int end = std::min(totalCount - 1, first + count + 15);
    if (start != lastSubStart || end != lastSubEnd) {
        lastSubStart = start;
        lastSubEnd = end;
        currentMemberListManager->updateSubscriptionRange(start, end);
        scheduleUpdate();
    }
}

void MemberListController::updateMemberList()
{
    if (!regionRoot) return;
    if (!shell.isMemberListVisible()) {
        regionRoot->SetInnerRML("");
        requestRender();
        return;
    }
    if (activeGuildId.isValid()) {
        renderGuildMemberList();
        return;
    }
    if (currentInstance && activeChannelId.isValid()) {
        if (auto channel = currentInstance->getChannel(activeChannelId)) {
            std::vector<Snowflake> recipients;
            if (channel->recipients.hasValue()) {
                for (const auto &user : channel->recipients.get())
                    if (user.id.hasValue() && user.id.get().isValid()) recipients.push_back(user.id.get());
            } else if (channel->recipientIds.hasValue()) {
                recipients = channel->recipientIds.get();
            }
            if (recipients.size() > 1 || channel->type.valueOr(Discord::ChannelType::DM) == Discord::ChannelType::GROUP_DM) {
                renderGroupDmList(recipients);
                return;
            }
            if (recipients.size() == 1) {
                renderSingleDmProfile(recipients.front());
                return;
            }
        }
    }
    regionRoot->SetInnerRML("");
    requestRender();
}

void MemberListController::renderGuildMemberList()
{
    if (!regionRoot || !currentMemberListManager || currentMemberListManager->totalItemCount() == 0) {
        if (regionRoot) regionRoot->SetInnerRML("");
        requestRender();
        return;
    }
    const int totalCount = currentMemberListManager->totalItemCount();
    Snowflake ownerId;
    if (currentInstance) if (auto guild = currentInstance->getGuild(activeGuildId)) ownerId = guild->ownerId.get();
    const float height = regionRoot->GetClientHeight() > 0 ? regionRoot->GetClientHeight() : 800.0f;
    const int first = static_cast<int>(regionRoot->GetScrollTop() / RowHeight);
    const int count = static_cast<int>(std::ceil(height / RowHeight)) + 1;
    const int start = std::max(0, first - 10);
    const int end = std::min(totalCount - 1, first + count + 10);
    if (start != lastSubStart || end != lastSubEnd) {
        lastSubStart = start;
        lastSubEnd = end;
        currentMemberListManager->updateSubscriptionRange(start, end);
    }
    std::string rml = "<div class=\"members-scroll-area\" style=\"height: " + std::to_string(totalCount * RowHeight) + "dp;\">";
    for (int i = start; i <= end; ++i) {
        const auto *item = currentMemberListManager->itemAt(i);
        const auto top = std::to_string(i * RowHeight);
        if (!item || item->type == Core::MemberListItem::Type::Placeholder) {
            rml += "<div class=\"member-item member-row\" style=\"top: " + top + "dp;\"><div class=\"member-avatar-wrap\"><div class=\"member-avatar-fallback\"></div></div><div class=\"member-content\"><div class=\"member-name\" style=\"opacity: 0.3;\">...</div></div></div>";
        } else if (item->type == Core::MemberListItem::Type::Group) {
            rml += "<div class=\"member-item member-group-header\" style=\"top: " + top + "dp;\">" + escape(Platform::unicodeUpper(item->groupName)) + " â€” " + std::to_string(item->groupCount) + "</div>";
        } else if (item->type == Core::MemberListItem::Type::Member) {
            const auto uid = item->userId;
            std::string userHash;
            if (item->member.user.hasValue() && item->member.user->avatar.hasValue()) userHash = item->member.user->avatar.get();
            const auto memberHash = item->member.avatar.valueOr();
            const auto url = Discord::Cdn::effectiveAvatar(activeGuildId, uid, memberHash, userHash, 64);
            const auto badge = currentPresenceManager ? currentPresenceManager->badge(uid, activeGuildId) : Core::PresenceBadge{};
            const auto activity = activityText(currentPresenceManager.get(), uid, activeGuildId);
            const auto color = item->roleColor.valid ? "color: " + item->roleColor.css() + ";" : std::string();
            rml += "<div class=\"member-item member-row " + std::string(badge.status == Discord::StatusType::OFFLINE ? "offline" : "") + "\" style=\"top: " + top + "dp;\" data-user-id=\"" + uid.toString() + "\"><div class=\"member-avatar-wrap\"><div class=\"member-avatar-clip\"><img class=\"member-avatar\" src=\"" + escape(shell.imageSource(url, {32, 32})) + "\"/></div><div class=\"member-status-dot " + statusClass(badge) + "\"></div></div><div class=\"member-content\"><div class=\"member-name-row\"><div class=\"member-name\" style=\"" + color + "\">" + escape(item->displayName) + "</div>";
            if (ownerId.isValid() && uid == ownerId) rml += "<img class=\"member-crown\" src=\"icon:crown\"/>";
            rml += "</div>";
            if (!activity.empty()) rml += "<div class=\"member-activity\">" + richText(activity) + "</div>";
            rml += "</div></div>";
        }
    }
    rml += "</div>";
    regionRoot->SetInnerRML(rml);
    requestRender();
}

void MemberListController::renderGroupDmList(const std::vector<Snowflake> &recipients)
{
    if (!regionRoot || !currentInstance) {
        if (regionRoot) regionRoot->SetInnerRML("");
        requestRender();
        return;
    }
    auto *users = currentInstance->users();
    std::string rml = "<div class=\"member-group-header\">MEMBERS â€” " + std::to_string(recipients.size()) + "</div>";
    for (auto uid : recipients) {
        auto name = users ? users->getDisplayName(uid) : uid.toString();
        if (name.empty()) name = uid.toString();
        std::string url;
        if (users) if (auto user = users->getUser(uid))
            if (user->avatar.hasValue() && !user->avatar->empty()) url = Discord::Cdn::userAvatar(uid, user->avatar.get(), 64);
        if (url.empty()) url = Discord::Cdn::defaultUserAvatar(uid, 64);
        const auto badge = currentPresenceManager ? currentPresenceManager->badge(uid) : Core::PresenceBadge{};
        const auto activity = activityText(currentPresenceManager.get(), uid);
        rml += "<div class=\"member-row " + std::string(badge.status == Discord::StatusType::OFFLINE ? "offline" : "") + "\" data-user-id=\"" + uid.toString() + "\" style=\"position: relative; margin-bottom: 2dp;\"><div class=\"member-avatar-wrap\"><div class=\"member-avatar-clip\"><img class=\"member-avatar\" src=\"" + escape(shell.imageSource(url, {32, 32})) + "\"/></div><div class=\"member-status-dot " + statusClass(badge) + "\"></div></div><div class=\"member-content\"><div class=\"member-name-row\"><div class=\"member-name\">" + escape(name) + "</div></div>";
        if (!activity.empty()) rml += "<div class=\"member-activity\">" + richText(activity) + "</div>";
        rml += "</div></div>";
    }
    regionRoot->SetInnerRML(rml);
    requestRender();
}

void MemberListController::renderSingleDmProfile(Snowflake targetUserId)
{
    if (!regionRoot || !currentInstance) {
        if (regionRoot) regionRoot->SetInnerRML("");
        requestRender();
        return;
    }
    auto *users = currentInstance->users();
    auto user = users ? users->getUser(targetUserId) : std::nullopt;
    std::string displayName = targetUserId.toString(), username = displayName;
    std::string url, bannerColor = "#19191c";
    if (user) {
        displayName = user->getDisplayName();
        username = user->tag();
        if (user->avatar.hasValue() && !user->avatar->empty()) url = Discord::Cdn::userAvatar(targetUserId, user->avatar.get(), 128);
        if (user->accentColor.hasValue() && user->accentColor.get() != 0) bannerColor = Platform::Color::rgb(user->accentColor.get()).css();
    }
    if (url.empty()) url = Discord::Cdn::defaultUserAvatar(targetUserId, 128);
    const auto badge = currentPresenceManager ? currentPresenceManager->badge(targetUserId) : Core::PresenceBadge{};
    const auto activity = activityText(currentPresenceManager.get(), targetUserId);
    std::string rml = "<div class=\"dm-profile-panel\" data-user-id=\"" + targetUserId.toString() + "\"><div class=\"dm-profile-banner\" style=\"background-color: " + bannerColor + ";\"></div><div class=\"dm-profile-body\"><div class=\"dm-profile-avatar-wrap\"><div class=\"dm-profile-avatar-clip\"><img class=\"dm-profile-avatar\" src=\"" + escape(shell.imageSource(url, {80, 80})) + "\"/></div><div class=\"dm-profile-status-dot " + statusClass(badge) + "\"></div></div><div class=\"dm-profile-display-name\">" + escape(displayName) + "</div><div class=\"dm-profile-username\">" + escape(username) + "</div>";
    if (!activity.empty()) rml += "<div class=\"dm-profile-divider\"></div><div class=\"dm-profile-section-title\">Activity</div><div class=\"dm-profile-activity\">" + richText(activity) + "</div>";
    rml += "<button class=\"dm-profile-view\" data-profile-view=\"true\">View Profile</button></div></div>";
    regionRoot->SetInnerRML(rml);
    requestRender();
}

} // namespace Styx::UI::Rml
