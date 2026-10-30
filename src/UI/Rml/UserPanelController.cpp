#include "UserPanelController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"

#include "Core/AccountInfo.hpp"
#ifndef STYX_NO_VOICE
#include "Core/Audio/VoiceManager.hpp"
#endif
#include "Core/ClientInstance.hpp"
#include "Core/Presence/ActivityFormat.hpp"
#include "Core/Presence/PresenceBadge.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "UI/ChannelList/ChannelNode.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"

#ifdef STYX_HAVE_SCREENSHARE
#include "Core/Stream/StreamManager.hpp"
#endif

#include <format>
#include <string_view>

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

namespace Styx {
namespace UI {
namespace Rml {

using Core::Snowflake;

UserPanelController::UserPanelController(Shell &shell, ::Rml::Element *root)
    : Controller(shell, root)
{
    if (auto *doc = shell.document()) {
        voicePanel = doc->GetElementById("voice-panel");
        userPanel = doc->GetElementById("user-panel");
    }


    if (userPanel) {
        listen("click", [this](::Rml::Event &event) {
            auto *target = event.GetTargetElement();
            if (auto *btn = closest(target, "data-action")) {
                const std::string action = attribute(btn, "data-action");
                if (action == std::string_view("toggle-mute")) {
                    this->shell.setSelfMute(!this->shell.isSelfMute());
                } else if (action == std::string_view("toggle-deaf")) {
                    this->shell.setSelfDeaf(!this->shell.isSelfDeaf());
                } else if (action == std::string_view("settings")) {
                    this->shell.openSettings();
                } else if (action == std::string_view("profile")) {
                    if (auto *inst = this->shell.currentInstance()) {
                        this->shell.showUserProfile(inst->accountId(), this->shell.location().guildId);
                    }
                }
            }
        }, userPanel);
    }

    if (voicePanel) {
        listen("click", [this](::Rml::Event &event) {
            auto *target = event.GetTargetElement();
            if (auto *btn = closest(target, "data-action")) {
                const std::string action = attribute(btn, "data-action");
                if (action == std::string_view("disconnect")) {
                    this->shell.leaveVoice();
                } else if (action == std::string_view("open-call")) {
                    this->shell.setCallOpen(true);
                } else if (action == std::string_view("toggle-share")) {
#ifdef STYX_HAVE_SCREENSHARE
                    this->onShareClicked();
#endif
                }
            }
        }, voicePanel);
    }

    track(shell.instanceChanged.connect([this](Core::ClientInstance *inst) {
        connectInstanceVoice(inst);
        scheduleRebuild();
    }));
    track(shell.locationChanged.connect([this](const Location &) { scheduleRebuild(); }));
    track(shell.accountReady.connect([this](Snowflake) { scheduleRebuild(); }));
    track(shell.selfMuteChanged.connect([this](bool) { scheduleRebuild(); }));
    track(shell.selfDeafChanged.connect([this](bool) { scheduleRebuild(); }));

    connectInstanceVoice(shell.currentInstance());
    rebuild();
}

void UserPanelController::connectInstanceVoice(Core::ClientInstance *instance)
{
    if (lastInstance.get() == instance)
        return;
    instanceConnections.clear();
    lastInstance = instance;
    if (!instance)
        return;

    instanceConnections.push_back(instance->detailsUpdated.connect([this](const Core::AccountInfo &) { scheduleRebuild(); }));
    instanceConnections.push_back(instance->voiceStateChanged.connect([this](Snowflake, Snowflake) { scheduleRebuild(); }));

    if (auto *pres = instance->presences()) {
        instanceConnections.push_back(pres->presencesChanged.connect([this](std::vector<Snowflake>) { scheduleRebuild(); }));
    }

#ifndef STYX_NO_VOICE
    if (auto *vm = instance->voice()) {
        instanceConnections.push_back(vm->voiceConnected.connect([this]() { scheduleRebuild(); }));
        instanceConnections.push_back(vm->voiceDisconnected.connect([this]() { scheduleRebuild(); }));
        instanceConnections.push_back(vm->voiceStateChanged.connect([this]() { scheduleRebuild(); }));
    }
#endif

#ifdef STYX_HAVE_SCREENSHARE
    if (auto *sm = instance->streams()) {
        instanceConnections.push_back(sm->streamingChanged.connect([this](bool) { scheduleRebuild(); }));
    }
#endif
}

void UserPanelController::scheduleRebuild()
{
    rebuildTimer.start(std::chrono::milliseconds(0), false, [this]() { rebuild(); });
}

void UserPanelController::rebuild()
{
    renderVoicePanel();
    renderUserPanel();
    requestRender(200);
}

void UserPanelController::renderVoicePanel()
{
    if (!voicePanel)
        return;

    bool inVoice = false;
    bool connected = false;
    Snowflake currentChannelId;
    Snowflake currentGuildId;

#ifndef STYX_NO_VOICE
    auto *inst = shell.currentInstance();
    if (inst) {
        if (auto *vm = inst->voice()) {
            const auto clientState = vm->clientState();
            inVoice = (clientState != Discord::Voice::VoiceClient::State::Disconnected);
            connected = (clientState == Discord::Voice::VoiceClient::State::Connected);
            currentChannelId = vm->currentChannelId();
            currentGuildId = vm->currentGuildId();
        }
    }
#endif

    if (!inVoice) {
        voicePanel->SetInnerRML("");
        voicePanel->SetProperty("display", "none");
        return;
    }

    voicePanel->SetProperty("display", "flex");

    const std::string statusText = connected ? "Voice Connected" : "Connecting...";
    const std::string statusClass = connected ? "voice-conn-status" : "voice-conn-status connecting";

    // Resolve channel and guild names
    std::string channelName = "Voice";
    std::string guildName;
    if (auto *inst = shell.currentInstance()) {
        auto chOpt = inst->getChannel(currentChannelId);
        if (chOpt.has_value()) {
            channelName = chOpt->name;
        } else if (auto *tree = shell.channelTree()) {
            if (auto *node = tree->findChannelTreeNode(currentChannelId, inst->accountId()))
                channelName = node->name;
        }

        if (currentGuildId.isValid()) {
            auto gOpt = inst->getGuild(currentGuildId);
            if (gOpt.has_value()) {
                guildName = gOpt->name;
            } else if (auto *tree = shell.channelTree()) {
                if (auto *node = tree->serverNode(inst->accountId(), currentGuildId))
                    guildName = node->name;
            }
        }
    }

    const std::string channelLabel = !guildName.empty()
                                     ? std::format("{0} / {1}", channelName, guildName)
                                     : channelName;

    std::string rml;
    rml += "<div class=\"voice-panel-header\">";
    rml += std::format("<span class=\"{0}\">{1}</span>", statusClass, Rml::escape(statusText));
    rml += "<div class=\"voice-action-btn disconnect\" data-action=\"disconnect\" title=\"Disconnect\">";
    rml += "<img src=\"icon:phone-off\"/>";
    rml += "</div>"; // .disconnect
    rml += "</div>"; // .voice-panel-header

    rml += "<div class=\"voice-panel-info\">";
    rml += std::format("<div class=\"voice-channel-link\" data-action=\"open-call\" title=\"Open Call\">{0}</div>", Rml::escape(channelLabel));

#ifdef STYX_HAVE_SCREENSHARE
    if (auto *inst = shell.currentInstance()) {
        auto *sm = inst->streams();
        const bool isStreaming = sm && sm->isStreaming();
        const std::string shareClass = isStreaming ? "voice-action-btn streaming" : "voice-action-btn";
        const std::string shareTip = isStreaming ? "Stop streaming" : "Share your screen";

        rml += std::format("<div class=\"{0}\" data-action=\"toggle-share\" title=\"{1}\">", shareClass, Rml::escape(shareTip));
        rml += "<img src=\"icon:monitor\"/>";
        rml += "</div>"; // .voice-action-btn
    }
#endif

    rml += "</div>"; // .voice-panel-info

    voicePanel->SetInnerRML(rml.c_str());
}

void UserPanelController::renderUserPanel()
{
    if (!userPanel)
        return;

    auto *inst = shell.currentInstance();
    if (!inst) {
        userPanel->SetInnerRML("");
        return;
    }

    const auto &info = inst->accountInfo();
    const Snowflake accountId = inst->accountId();
    auto *userMgr = inst->users();
    std::optional<Discord::User> me;
    if (userMgr)
        me = userMgr->getUser(accountId);

    std::string displayName = (me && me->globalName.hasValue() && !me->globalName->empty())
                              ? *me->globalName
                              : (info.displayName.empty() ? info.username : info.displayName);
    if (displayName.empty())
        displayName = info.username;

    // Sub-name: display custom status if active, otherwise username
    auto *pres = inst->presences();
    std::string customStatus;
    if (pres) {
        const auto activities = pres->activities(accountId);
        if (const auto *activity = Core::ActivityFormat::custom(activities))
            customStatus = activity->stateText();
    }
    std::string subName = !customStatus.empty() ? customStatus : (me ? "@" + me->username.get() : "@" + info.username);

    std::string avatarHash = (me && me->avatar.hasValue()) ? *me->avatar : info.avatar;
    std::string avatarUrl = !avatarHash.empty()
                         ? Discord::Cdn::userAvatar(accountId, avatarHash, 32)
                         : Discord::Cdn::defaultUserAvatar(accountId);
    std::string avatarSrc = shell.imageSource(avatarUrl, Platform::Size{32, 32});

    // Presence status dot
    auto badge = pres ? pres->badge(accountId) : Core::PresenceBadge();
    std::string statusClass;
    switch (badge.status) {
    case Discord::StatusType::ONLINE: statusClass = "status-online"; break;
    case Discord::StatusType::IDLE: statusClass = "status-idle"; break;
    case Discord::StatusType::DND: statusClass = "status-dnd"; break;
    case Discord::StatusType::OFFLINE:
    default: statusClass = "status-offline"; break;
    }
    if (badge.streaming)
        statusClass = "status-streaming";

    // Mute and deaf state (from Shell and VoiceManager)
    bool isMuted = shell.isSelfMute();
    bool isDeafened = shell.isSelfDeaf();
#ifndef STYX_NO_VOICE
    if (auto *vm = inst->voice()) {
        auto vs = vm->voiceStateForUser(accountId);
        if (vs.has_value()) {
            if (vs->selfMute.hasValue() && vs->selfMute.get())
                isMuted = true;
            if (vs->selfDeaf.hasValue() && vs->selfDeaf.get())
                isDeafened = true;
        }
    }
#endif

    const std::string muteClass = isMuted ? "panel-btn muted" : "panel-btn";
    const std::string muteIcon = isMuted ? "icon:mic-off" : "icon:mic";
    const std::string muteTip = isMuted ? "Unmute" : "Mute";

    const std::string deafClass = isDeafened ? "panel-btn deafened" : "panel-btn";
    const std::string deafIcon = isDeafened ? "icon:volume-x" : "icon:headphones";
    const std::string deafTip = isDeafened ? "Undeafen" : "Deafen";

    std::string rml;
    rml += "<div class=\"user-panel-info\" data-action=\"profile\" title=\"User Profile\">";
    rml += "<div class=\"user-avatar-wrap\">";
    rml += "<div class=\"user-avatar-clip\">";
    rml += std::format("<img class=\"user-avatar\" src=\"{0}\"/>", Rml::escape(avatarSrc));
    rml += "</div>"; // .user-avatar-clip
    rml += std::format("<div class=\"status-dot {0}\"></div>", statusClass);
    rml += "</div>"; // .user-avatar-wrap

    rml += "<div class=\"user-names\">";
    rml += std::format("<span class=\"user-display-name\">{0}</span>", Rml::escape(displayName));
    rml += std::format("<span class=\"user-sub-name\">{0}</span>", Rml::escape(subName));
    rml += "</div>"; // .user-names
    rml += "</div>"; // .user-panel-info

    rml += "<div class=\"user-panel-buttons\">";
    rml += std::format("<div class=\"{0}\" data-action=\"toggle-mute\" title=\"{1}\">", muteClass, Rml::escape(muteTip));
    rml += std::format("<img src=\"{0}\"/>", muteIcon);
    rml += "</div>"; // mute-btn

    rml += std::format("<div class=\"{0}\" data-action=\"toggle-deaf\" title=\"{1}\">", deafClass, Rml::escape(deafTip));
    rml += std::format("<img src=\"{0}\"/>", deafIcon);
    rml += "</div>"; // deaf-btn

    rml += "<div class=\"panel-btn\" data-action=\"settings\" title=\"User Settings\">";
    rml += "<img src=\"icon:settings\"/>";
    rml += "</div>"; // settings-btn
    rml += "</div>"; // .user-panel-buttons

    userPanel->SetInnerRML(rml.c_str());
}

#ifdef STYX_HAVE_SCREENSHARE
void UserPanelController::onShareClicked()
{
    auto *inst = shell.currentInstance();
    if (!inst)
        return;
    auto *streamMgr = inst->streams();
    if (!streamMgr)
        return;

    if (streamMgr->isStreaming()) {
        streamMgr->stopStreaming();
        return;
    }

    shell.openGoLivePicker();
}
#endif

} // namespace Rml
} // namespace UI
} // namespace Styx
