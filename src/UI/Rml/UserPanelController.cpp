#include "UserPanelController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"

#include "Core/AccountInfo.hpp"
#include "Core/Audio/VoiceManager.hpp"
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

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

namespace Styx {
namespace UI {
namespace Rml {

using Core::Snowflake;

UserPanelController::UserPanelController(Shell &shell, ::Rml::Element *root, QObject *parent)
    : Controller(shell, root, parent)
{
    if (auto *doc = shell.document()) {
        voicePanel = doc->GetElementById("voice-panel");
        userPanel = doc->GetElementById("user-panel");
    }

    rebuildTimer.setSingleShot(true);
    rebuildTimer.setInterval(0);
    connect(&rebuildTimer, &QTimer::timeout, this, &UserPanelController::rebuild);

    if (userPanel) {
        listen("click", [this](::Rml::Event &event) {
            auto *target = event.GetTargetElement();
            if (auto *btn = closest(target, "data-action")) {
                const QString action = attribute(btn, "data-action");
                if (action == QLatin1String("toggle-mute")) {
                    this->shell.setSelfMute(!this->shell.isSelfMute());
                } else if (action == QLatin1String("toggle-deaf")) {
                    this->shell.setSelfDeaf(!this->shell.isSelfDeaf());
                } else if (action == QLatin1String("settings")) {
                    this->shell.openSettings();
                } else if (action == QLatin1String("profile")) {
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
                const QString action = attribute(btn, "data-action");
                if (action == QLatin1String("disconnect")) {
                    this->shell.leaveVoice();
                } else if (action == QLatin1String("open-call")) {
                    this->shell.setCallOpen(true);
                } else if (action == QLatin1String("toggle-share")) {
#ifdef STYX_HAVE_SCREENSHARE
                    this->onShareClicked();
#endif
                }
            }
        }, voicePanel);
    }

    connect(&shell, &Shell::instanceChanged, this, [this](Core::ClientInstance *inst) {
        connectInstanceVoice(inst);
        rebuildTimer.start();
    });

    connect(&shell, &Shell::locationChanged, this, [this]() {
        rebuildTimer.start();
    });

    connect(&shell, &Shell::accountReady, this, [this]() {
        rebuildTimer.start();
    });

    connect(&shell, &Shell::selfMuteChanged, this, [this]() {
        rebuildTimer.start();
    });

    connect(&shell, &Shell::selfDeafChanged, this, [this]() {
        rebuildTimer.start();
    });

    connectInstanceVoice(shell.currentInstance());
    rebuild();
}

void UserPanelController::connectInstanceVoice(Core::ClientInstance *instance)
{
    if (lastInstance == instance)
        return;
    lastInstance = instance;
    if (!instance)
        return;

    connect(instance, &Core::ClientInstance::detailsUpdated, this, [this]() { rebuildTimer.start(); });
    connect(instance, &Core::ClientInstance::voiceStateChanged, this, [this]() { rebuildTimer.start(); });

    if (auto *pres = instance->presences()) {
        connect(pres, &Core::PresenceManager::presencesChanged, this, [this]() { rebuildTimer.start(); });
    }

#ifndef STYX_NO_VOICE
    if (auto *vm = instance->voice()) {
        connect(vm, &Core::Audio::VoiceManager::voiceConnected, this, [this]() { rebuildTimer.start(); });
        connect(vm, &Core::Audio::VoiceManager::voiceDisconnected, this, [this]() { rebuildTimer.start(); });
        connect(vm, &Core::Audio::VoiceManager::voiceStateChanged, this, [this]() { rebuildTimer.start(); });
    }
#endif

#ifdef STYX_HAVE_SCREENSHARE
    if (auto *sm = instance->streams()) {
        connect(sm, &Core::Stream::StreamManager::streamingChanged, this, [this]() { rebuildTimer.start(); });
    }
#endif
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
    Discord::Voice::VoiceClient::State clientState = Discord::Voice::VoiceClient::State::Disconnected;
    Snowflake currentChannelId;
    Snowflake currentGuildId;

#ifndef STYX_NO_VOICE
    auto *inst = shell.currentInstance();
    if (inst) {
        if (auto *vm = inst->voice()) {
            clientState = vm->clientState();
            inVoice = (clientState != Discord::Voice::VoiceClient::State::Disconnected);
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

    const bool connected = (clientState == Discord::Voice::VoiceClient::State::Connected);
    const QString statusText = connected ? QStringLiteral("Voice Connected") : QStringLiteral("Connecting...");
    const QString statusClass = connected ? QStringLiteral("voice-conn-status") : QStringLiteral("voice-conn-status connecting");

    // Resolve channel and guild names
    QString channelName = QStringLiteral("Voice");
    QString guildName;
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
                const QModelIndex sIdx = tree->serverIndex(inst->accountId(), currentGuildId);
                guildName = sIdx.data(Qt::DisplayRole).toString();
            }
        }
    }

    const QString channelLabel = !guildName.isEmpty()
                                     ? QStringLiteral("%1 / %2").arg(channelName).arg(guildName)
                                     : channelName;

    QString rml;
    rml += QStringLiteral("<div class=\"voice-panel-header\">");
    rml += QStringLiteral("<span class=\"%1\">%2</span>").arg(statusClass).arg(Rml::escape(statusText));
    rml += QStringLiteral("<div class=\"voice-action-btn disconnect\" data-action=\"disconnect\" title=\"Disconnect\">");
    rml += QStringLiteral("<img src=\"icon:phone-off\"/>");
    rml += QStringLiteral("</div>"); // .disconnect
    rml += QStringLiteral("</div>"); // .voice-panel-header

    rml += QStringLiteral("<div class=\"voice-panel-info\">");
    rml += QStringLiteral("<div class=\"voice-channel-link\" data-action=\"open-call\" title=\"Open Call\">%1</div>")
               .arg(Rml::escape(channelLabel));

#ifdef STYX_HAVE_SCREENSHARE
    if (auto *inst = shell.currentInstance()) {
        auto *sm = inst->streams();
        const bool isStreaming = sm && sm->isStreaming();
        const QString shareClass = isStreaming ? QStringLiteral("voice-action-btn streaming") : QStringLiteral("voice-action-btn");
        const QString shareTip = isStreaming ? QStringLiteral("Stop streaming") : QStringLiteral("Share your screen");

        rml += QStringLiteral("<div class=\"%1\" data-action=\"toggle-share\" title=\"%2\">")
                   .arg(shareClass)
                   .arg(Rml::escape(shareTip));
        rml += QStringLiteral("<img src=\"icon:monitor\"/>");
        rml += QStringLiteral("</div>"); // .voice-action-btn
    }
#endif

    rml += QStringLiteral("</div>"); // .voice-panel-info

    voicePanel->SetInnerRML(rml.toUtf8().constData());
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

    QString displayName = (me && me->globalName.hasValue() && !me->globalName->isEmpty())
                              ? *me->globalName
                              : (info.displayName.isEmpty() ? info.username : info.displayName);
    if (displayName.isEmpty())
        displayName = info.username;

    // Sub-name: display custom status if active, otherwise username
    auto *pres = inst->presences();
    QString customStatus;
    if (pres) {
        const auto activities = pres->activities(accountId);
        if (const auto *activity = Core::ActivityFormat::custom(activities))
            customStatus = activity->stateText();
    }
    QString subName = !customStatus.isEmpty() ? customStatus : (me ? QStringLiteral("@") + me->username : QStringLiteral("@") + info.username);

    QString avatarHash = (me && me->avatar.hasValue()) ? *me->avatar : info.avatar;
    QUrl avatarUrl = !avatarHash.isEmpty()
                         ? Discord::Cdn::userAvatar(accountId, avatarHash, 32)
                         : Discord::Cdn::defaultUserAvatar(accountId);
    QString avatarSrc = shell.imageSource(avatarUrl, QSize(32, 32));

    // Presence status dot
    auto badge = pres ? pres->badge(accountId) : Core::PresenceBadge();
    QString statusClass;
    switch (badge.status) {
    case Discord::StatusType::ONLINE: statusClass = QStringLiteral("status-online"); break;
    case Discord::StatusType::IDLE: statusClass = QStringLiteral("status-idle"); break;
    case Discord::StatusType::DND: statusClass = QStringLiteral("status-dnd"); break;
    case Discord::StatusType::OFFLINE:
    default: statusClass = QStringLiteral("status-offline"); break;
    }
    if (badge.streaming)
        statusClass = QStringLiteral("status-streaming");

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

    const QString muteClass = isMuted ? QStringLiteral("panel-btn muted") : QStringLiteral("panel-btn");
    const QString muteIcon = isMuted ? QStringLiteral("icon:mic-off") : QStringLiteral("icon:mic");
    const QString muteTip = isMuted ? QStringLiteral("Unmute") : QStringLiteral("Mute");

    const QString deafClass = isDeafened ? QStringLiteral("panel-btn deafened") : QStringLiteral("panel-btn");
    const QString deafIcon = isDeafened ? QStringLiteral("icon:volume-x") : QStringLiteral("icon:headphones");
    const QString deafTip = isDeafened ? QStringLiteral("Undeafen") : QStringLiteral("Deafen");

    QString rml;
    rml += QStringLiteral("<div class=\"user-panel-info\" data-action=\"profile\" title=\"User Profile\">");
    rml += QStringLiteral("<div class=\"user-avatar-wrap\">");
    rml += QStringLiteral("<div class=\"user-avatar-clip\">");
    rml += QStringLiteral("<img class=\"user-avatar\" src=\"%1\"/>").arg(Rml::escape(avatarSrc));
    rml += QStringLiteral("</div>"); // .user-avatar-clip
    rml += QStringLiteral("<div class=\"status-dot %1\"></div>").arg(statusClass);
    rml += QStringLiteral("</div>"); // .user-avatar-wrap

    rml += QStringLiteral("<div class=\"user-names\">");
    rml += QStringLiteral("<span class=\"user-display-name\">%1</span>").arg(Rml::escape(displayName));
    rml += QStringLiteral("<span class=\"user-sub-name\">%1</span>").arg(Rml::escape(subName));
    rml += QStringLiteral("</div>"); // .user-names
    rml += QStringLiteral("</div>"); // .user-panel-info

    rml += QStringLiteral("<div class=\"user-panel-buttons\">");
    rml += QStringLiteral("<div class=\"%1\" data-action=\"toggle-mute\" title=\"%2\">").arg(muteClass).arg(Rml::escape(muteTip));
    rml += QStringLiteral("<img src=\"%1\"/>").arg(muteIcon);
    rml += QStringLiteral("</div>"); // mute-btn

    rml += QStringLiteral("<div class=\"%1\" data-action=\"toggle-deaf\" title=\"%2\">").arg(deafClass).arg(Rml::escape(deafTip));
    rml += QStringLiteral("<img src=\"%1\"/>").arg(deafIcon);
    rml += QStringLiteral("</div>"); // deaf-btn

    rml += QStringLiteral("<div class=\"panel-btn\" data-action=\"settings\" title=\"User Settings\">");
    rml += QStringLiteral("<img src=\"icon:settings\"/>");
    rml += QStringLiteral("</div>"); // settings-btn
    rml += QStringLiteral("</div>"); // .user-panel-buttons

    userPanel->SetInnerRML(rml.toUtf8().constData());
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
