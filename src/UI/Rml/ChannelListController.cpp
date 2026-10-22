#include "ChannelListController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Presence/PresenceBadge.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Core/Presence/ActivityFormat.hpp"
#include "Discord/CdnUrls.hpp"
#include "UI/ChannelList/ChannelFilterProxyModel.hpp"
#include "UI/ChannelList/ChannelNode.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"

#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

#include <QSettings>

namespace Acheron {
namespace UI {
namespace Rml {

using Core::Snowflake;

ChannelListController::ChannelListController(Shell &shell, ::Rml::Element *root, QObject *parent)
    : Controller(shell, root, parent)
{
    if (auto *doc = shell.document()) {
        sidebarHeader = doc->GetElementById("sidebar-header");
        channelList = doc->GetElementById("channel-list");
    }

    rebuildTimer.setSingleShot(true);
    rebuildTimer.setInterval(0);
    connect(&rebuildTimer, &QTimer::timeout, this, &ChannelListController::rebuild);

    if (sidebarHeader) {
        listen("click", [this](::Rml::Event &event) {
            auto *target = event.GetTargetElement();
            if (auto *act = closest(target, "data-action")) {
                QString a = attribute(act, "data-action");
                if (a == QLatin1String("clear-dm-search")) {
                    dmFilterText.clear();
                    if (auto *inp = dynamic_cast<::Rml::ElementFormControlInput *>(sidebarHeader->GetElementById("sidebar-search-input"))) {
                        inp->SetValue("");
                    }
                    rebuildTimer.start();
                    return;
                }
            }
            if (!this->shell.location().isHome()) {
                this->shell.showContextMenuForGuild(this->shell.location().accountId, this->shell.location().guildId);
            }
        }, sidebarHeader);

        listen("change", [this](::Rml::Event &event) {
            auto *target = event.GetTargetElement();
            if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target)) {
                if (input->GetId() == "sidebar-search-input") {
                    QString val = QString::fromStdString(input->GetValue());
                    if (val != dmFilterText) {
                        dmFilterText = val;
                        rebuildTimer.start();
                    }
                }
            }
        }, sidebarHeader);

        listen("keyup", [this](::Rml::Event &event) {
            auto *target = event.GetTargetElement();
            if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target)) {
                if (input->GetId() == "sidebar-search-input") {
                    QString val = QString::fromStdString(input->GetValue());
                    if (val != dmFilterText) {
                        dmFilterText = val;
                        rebuildTimer.start();
                    }
                }
            }
        }, sidebarHeader, true);
    }

    if (channelList) {
        listen("click", [this](::Rml::Event &event) {
            auto *target = event.GetTargetElement();
            if (auto *act = closest(target, "data-action")) {
                QString a = attribute(act, "data-action");
                if (a == QLatin1String("friends")) {
                    this->shell.openFriends(this->shell.location().accountId);
                    return;
                }
            }

            if (auto *cat = closest(target, "data-category-id")) {
                Snowflake catId(attribute(cat, "data-category-id").toULongLong());
                if (auto *tree = this->shell.channelTree()) {
                    if (auto *catNode = tree->findChannelTreeNode(catId, this->shell.location().accountId)) {
                        const bool newCollapsed = !catNode->collapsed;
                        const QModelIndex srcIdx = tree->indexForNode(catNode);
                        tree->setCollapsed(srcIdx, newCollapsed);

                        QSettings settings;
                        const QString key = QStringLiteral("category_collapsed/%1/%2")
                                                .arg(this->shell.location().accountId.toString())
                                                .arg(catId.toString());
                        settings.setValue(key, newCollapsed);
                    }
                }
                rebuildTimer.start();
                return;
            }

            if (auto *chan = closest(target, "data-channel-id")) {
                Snowflake chId(attribute(chan, "data-channel-id").toULongLong());
                QString type = attribute(chan, "data-type");
                if (type == QLatin1String("voice")) {
                    this->shell.joinVoice(this->shell.location().accountId, this->shell.location().guildId, chId);
                    this->shell.openChannel(this->shell.location().accountId, this->shell.location().guildId, chId);
                } else {
                    this->shell.openChannel(this->shell.location().accountId, this->shell.location().guildId, chId);
                }
                return;
            }

            if (auto *dm = closest(target, "data-dm-id")) {
                Snowflake chId(attribute(dm, "data-dm-id").toULongLong());
                this->shell.openChannel(this->shell.location().accountId, Snowflake::Invalid, chId);
                return;
            }

            if (auto *user = closest(target, "data-user-id")) {
                Snowflake uId(attribute(user, "data-user-id").toULongLong());
                this->shell.showUserProfile(uId, this->shell.location().guildId);
                return;
            }
        }, channelList);

        listen("mousedown", [this](::Rml::Event &event) {
            if (event.GetParameter<int>("button", 0) != 1) // Right click
                return;

            auto *target = event.GetTargetElement();
            if (auto *chan = closest(target, "data-channel-id")) {
                Snowflake chId(attribute(chan, "data-channel-id").toULongLong());
                this->shell.showContextMenuForChannel(this->shell.location().accountId, this->shell.location().guildId, chId);
                return;
            }

            if (auto *dm = closest(target, "data-dm-id")) {
                Snowflake chId(attribute(dm, "data-dm-id").toULongLong());
                this->shell.showContextMenuForChannel(this->shell.location().accountId, Snowflake::Invalid, chId);
                return;
            }

            if (auto *user = closest(target, "data-user-id")) {
                Snowflake uId(attribute(user, "data-user-id").toULongLong());
                this->shell.showContextMenuForUser(uId, this->shell.location().guildId);
                return;
            }
        }, channelList);
    }

    connect(&shell, &Shell::locationChanged, this, [this](const Location &loc) {
        if (auto *filter = this->shell.channelFilter()) {
            filter->setSelectedChannel(loc.channelId, loc.accountId);
            filter->invalidateFilter();
        }
        rebuildTimer.start();
    });

    connect(&shell, &Shell::instanceChanged, this, [this](Core::ClientInstance *inst) {
        connectInstanceVoice(inst);
        rebuildTimer.start();
    });

    connect(&shell, &Shell::accountReady, this, [this]() {
        rebuildTimer.start();
    });

    if (auto *tree = shell.channelTree()) {
        connect(tree, &QAbstractItemModel::modelReset, this, [this]() { rebuildTimer.start(); });
        connect(tree, &QAbstractItemModel::dataChanged, this, [this]() { rebuildTimer.start(); });
        connect(tree, &QAbstractItemModel::rowsInserted, this, [this]() { rebuildTimer.start(); });
        connect(tree, &QAbstractItemModel::rowsRemoved, this, [this]() { rebuildTimer.start(); });
        connect(tree, &QAbstractItemModel::layoutChanged, this, [this]() { rebuildTimer.start(); });
    }

    if (auto *filter = shell.channelFilter()) {
        connect(filter, &QAbstractItemModel::modelReset, this, [this]() { rebuildTimer.start(); });
        connect(filter, &QAbstractItemModel::dataChanged, this, [this]() { rebuildTimer.start(); });
        connect(filter, &QAbstractItemModel::rowsInserted, this, [this]() { rebuildTimer.start(); });
        connect(filter, &QAbstractItemModel::rowsRemoved, this, [this]() { rebuildTimer.start(); });
        connect(filter, &QAbstractItemModel::layoutChanged, this, [this]() { rebuildTimer.start(); });
    }

    connectInstanceVoice(shell.currentInstance());
    renderSidebar();
}

void ChannelListController::connectInstanceVoice(Core::ClientInstance *instance)
{
    if (lastInstance == instance)
        return;
    if (lastInstance) {
        disconnect(lastInstance->presences(), nullptr, this, nullptr);
#ifndef ACHERON_NO_VOICE
        if (auto *voice = lastInstance->voice())
            disconnect(voice, nullptr, this, nullptr);
#endif
    }
    lastInstance = instance;
    dmFilterText.clear();
    if (sidebarHeader) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(sidebarHeader->GetElementById("sidebar-search-input")))
            input->SetValue("");
    }
    if (!instance)
        return;
    connect(instance->presences(), &Core::PresenceManager::presencesChanged, this, [this] {
        if (this->shell.location().isHome())
            rebuildTimer.start();
    });

#ifndef ACHERON_NO_VOICE
    if (auto *vm = instance->voice()) {
        connect(vm, &Core::Audio::VoiceManager::channelVoiceMemberChanged, this, [this]() { rebuildTimer.start(); });
        connect(vm, &Core::Audio::VoiceManager::participantSpeakingChanged, this, [this]() { rebuildTimer.start(); });
        connect(vm, &Core::Audio::VoiceManager::participantVoiceStateChanged, this, [this]() { rebuildTimer.start(); });
        connect(vm, &Core::Audio::VoiceManager::participantJoined, this, [this]() { rebuildTimer.start(); });
        connect(vm, &Core::Audio::VoiceManager::participantLeft, this, [this]() { rebuildTimer.start(); });
        connect(vm, &Core::Audio::VoiceManager::participantUpdated, this, [this]() { rebuildTimer.start(); });
    }
#endif
}

void ChannelListController::rebuild()
{
    renderSidebar();
}

void ChannelListController::renderSidebar()
{
    if (!sidebarHeader || !channelList)
        return;

    if (shell.location().isHome()) {
        renderHomeSidebar();
    } else {
        renderGuildSidebar();
    }
    requestRender(200);
}

void ChannelListController::renderHomeSidebar()
{
    auto *existingInput = sidebarHeader->GetElementById("sidebar-search-input");
    if (!existingInput) {
        const QString clearBtn = QStringLiteral(
            "<img id=\"sidebar-search-clear\" class=\"sidebar-search-clear\" src=\"icon:x\" data-action=\"clear-dm-search\" style=\"%1\"/>")
            .arg(dmFilterText.isEmpty() ? QStringLiteral("display: none;") : QString());
        const QString headerRml = QStringLiteral(
            "<div class=\"sidebar-search-box\">"
            "<img class=\"search-icon\" src=\"icon:search\"/>"
            "<input type=\"text\" id=\"sidebar-search-input\" class=\"sidebar-search-input\" value=\"%1\"/>"
            "<span id=\"sidebar-search-placeholder\" class=\"sidebar-search-placeholder\" style=\"%2\">Find or start a conversation</span>"
            "%3"
            "</div>")
            .arg(Rml::escape(dmFilterText),
                 dmFilterText.isEmpty() ? QString() : QStringLiteral("display: none;"),
                 clearBtn);
        sidebarHeader->SetInnerRML(headerRml.toUtf8().constData());
    } else {
        if (auto *placeholder = sidebarHeader->GetElementById("sidebar-search-placeholder")) {
            placeholder->SetProperty("display", dmFilterText.isEmpty() ? "inline" : "none");
        }
    }
    if (auto *clear = sidebarHeader->GetElementById("sidebar-search-clear"))
        clear->SetProperty("display", dmFilterText.isEmpty() ? "none" : "block");
    QString rml;
    const bool friendsSelected = shell.isFriendsOpen();
    QString friendsRowClass = QStringLiteral("channel-row dm-friends-row");
    if (friendsSelected)
        friendsRowClass += QStringLiteral(" selected");

    rml += QStringLiteral(
        "<div class=\"%1\" data-action=\"friends\">"
        "<img class=\"channel-icon\" src=\"icon:user-plus\"/>"
        "<span class=\"channel-name\">Friends</span>"
        "</div>").arg(friendsRowClass);

    rml += QStringLiteral("<div class=\"section-header\">Direct Messages</div>");

    auto *tree = shell.channelTree();
    auto *filter = shell.channelFilter();
    if (tree && filter) {
        const QModelIndex srcDmHeader = tree->dmHeaderIndex(shell.location().accountId);
        const QModelIndex proxyDmHeader = filter->mapFromSource(srcDmHeader);
        if (proxyDmHeader.isValid()) {
            const int dmCount = filter->rowCount(proxyDmHeader);
            for (int i = 0; i < dmCount; ++i) {
                const QModelIndex dmIdx = filter->index(i, 0, proxyDmHeader);
                ChannelNode *node = tree->nodeFromIndex(filter->mapToSource(dmIdx));
                if (!node)
                    continue;

                if (!dmFilterText.trimmed().isEmpty()) {
                    if (!node->name.contains(dmFilterText.trimmed(), Qt::CaseInsensitive))
                        continue;
                }

                const bool isSelected = !friendsSelected && (node->id == shell.location().channelId);
                const bool isUnread = dmIdx.data(ChannelTreeModel::IsUnreadRole).toBool();
                const int mentions = dmIdx.data(ChannelTreeModel::MentionCountRole).toInt();

                QString avatarSrc;
                if (node->dmRecipientId.isValid()) {
                    QUrl url = Discord::Cdn::userAvatar(node->dmRecipientId, node->dmAvatarHash, 32);
                    avatarSrc = shell.imageSource(url, QSize(32, 32));
                } else if (!node->TEMP_iconHash.isEmpty()) {
                    QUrl url = Discord::Cdn::channelIcon(node->id, node->TEMP_iconHash, 32);
                    avatarSrc = shell.imageSource(url, QSize(32, 32));
                } else {
                    QUrl url = Discord::Cdn::defaultUserAvatar(node->dmRecipientId.isValid() ? node->dmRecipientId : node->id);
                    avatarSrc = shell.imageSource(url, QSize(32, 32));
                }

                auto badge = dmIdx.data(ChannelTreeModel::PresenceBadgeRole).value<Core::PresenceBadge>();
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

                QString rowClass = QStringLiteral("dm-row");
                if (isSelected) rowClass += QStringLiteral(" selected");
                if (isUnread) rowClass += QStringLiteral(" unread");

                rml += QStringLiteral("<div class=\"%1\" data-dm-id=\"%2\" data-recipient-id=\"%3\">")
                           .arg(rowClass)
                           .arg(node->id.toString())
                           .arg(node->dmRecipientId.toString());
                rml += QStringLiteral("<div class=\"dm-avatar-wrap\">");
                rml += QStringLiteral("<div class=\"avatar-clip\">");
                rml += QStringLiteral("<img class=\"dm-avatar\" src=\"%1\"/>").arg(Rml::escape(avatarSrc));
                rml += QStringLiteral("</div>"); // .avatar-clip
                rml += QStringLiteral("<div class=\"status-dot %1\"></div>").arg(statusClass);
                rml += QStringLiteral("</div>"); // .dm-avatar-wrap

                QString subtext;
                if (node->dmRecipientId.isValid()) {
                    if (auto *inst = shell.currentInstance()) {
                        auto acts = inst->presences()->activities(node->dmRecipientId);
                        auto sorted = Core::ActivityFormat::sortAndFilter(acts);
                        const auto *custom = Core::ActivityFormat::custom(acts);
                        const auto *prim = Core::ActivityFormat::primary(sorted);
                        if (custom && !custom->stateText().isEmpty()) {
                            subtext = custom->stateText();
                        } else if (prim) {
                            subtext = Core::ActivityFormat::cardHeading(*prim);
                            QString sec = Core::ActivityFormat::secondaryText(*prim);
                            if (!sec.isEmpty())
                                subtext += QStringLiteral(": ") + sec;
                        }
                    }
                } else if (!node->recipientIds.isEmpty()) {
                    subtext = QStringLiteral("%1 Members").arg(node->recipientIds.size() + 1);
                }

                rml += QStringLiteral("<div class=\"dm-text\">");
                rml += QStringLiteral("<span class=\"dm-name\">%1</span>").arg(Rml::escape(node->name));
                if (!subtext.isEmpty()) {
                    rml += QStringLiteral("<span class=\"dm-subtext\">%1</span>").arg(Rml::escape(subtext));
                }
                rml += QStringLiteral("</div>"); // .dm-text

                if (mentions > 0) {
                    QString badgeText = mentions > 99 ? QStringLiteral("99+") : QString::number(mentions);
                    rml += QStringLiteral("<div class=\"channel-mention-badge\">%1</div>").arg(badgeText);
                }

                rml += QStringLiteral("</div>"); // .dm-row
            }
        }
    }

    channelList->SetInnerRML(rml.toUtf8().constData());
}

void ChannelListController::renderGuildSidebar()
{
    auto *tree = shell.channelTree();
    auto *filter = shell.channelFilter();
    if (!tree || !filter)
        return;

    const Snowflake accountId = shell.location().accountId;
    const Snowflake guildId = shell.location().guildId;

    const QModelIndex srcGuild = tree->serverIndex(accountId, guildId);
    QString guildName = srcGuild.data(Qt::DisplayRole).toString();
    if (guildName.isEmpty())
        guildName = QStringLiteral("Server");

    // Header
    const QString headerRml = QStringLiteral(
        "<div class=\"guild-header-content\">"
        "<span class=\"guild-header-title\">%1</span>"
        "<img class=\"guild-header-chevron\" src=\"icon:chevron-down\"/>"
        "</div>").arg(Rml::escape(guildName));
    sidebarHeader->SetInnerRML(headerRml.toUtf8().constData());

    // Channels
    QString rml;
    const QModelIndex proxyGuild = filter->mapFromSource(srcGuild);
    if (proxyGuild.isValid()) {
        const int count = filter->rowCount(proxyGuild);
        for (int i = 0; i < count; ++i) {
            renderChannelNode(filter->index(i, 0, proxyGuild), rml, 0);
        }
    }

    channelList->SetInnerRML(rml.toUtf8().constData());
}

void ChannelListController::renderChannelNode(const QModelIndex &proxyIndex, QString &rml, int depth)
{
    auto *tree = shell.channelTree();
    auto *filter = shell.channelFilter();
    if (!tree || !filter || !proxyIndex.isValid())
        return;

    ChannelNode *node = tree->nodeFromIndex(filter->mapToSource(proxyIndex));
    if (!node)
        return;

    if (node->type == ChannelNode::Type::Category) {
        // Sync collapsed state from QSettings if stored
        QSettings settings;
        const QString key = QStringLiteral("category_collapsed/%1/%2")
                                .arg(shell.location().accountId.toString())
                                .arg(node->id.toString());
        if (settings.contains(key)) {
            const bool collapsed = settings.value(key).toBool();
            if (node->collapsed != collapsed) {
                tree->setCollapsed(filter->mapToSource(proxyIndex), collapsed);
            }
        }

        const QString chevronIcon = node->collapsed ? QStringLiteral("icon:chevron-right") : QStringLiteral("icon:chevron-down");
        rml += QStringLiteral(
            "<div class=\"category-row\" data-category-id=\"%1\">"
            "<img class=\"category-icon\" src=\"%2\"/>"
            "<span class=\"category-name\">%3</span>"
            "</div>")
            .arg(node->id.toString())
            .arg(chevronIcon)
            .arg(Rml::escape(node->name.toUpper()));

        // Children under category
        const int catChildCount = filter->rowCount(proxyIndex);
        for (int c = 0; c < catChildCount; ++c) {
            renderChannelNode(filter->index(c, 0, proxyIndex), rml, depth + 1);
        }
        return;
    }

    if (node->type == ChannelNode::Type::Channel || node->type == ChannelNode::Type::Forum) {
        const bool isForum = (node->type == ChannelNode::Type::Forum);
        const bool isSelected = (node->id == shell.location().channelId);
        const bool isUnread = node->isUnread;
        const bool isMuted = node->isMuted;
        const int mentions = node->mentionCount;

        QString iconName = QStringLiteral("icon:hash");
        if (isForum) {
            iconName = QStringLiteral("icon:messages-square");
        } else if (node->isAnnouncement) {
            iconName = QStringLiteral("icon:megaphone");
        } else if (node->isPrivate) {
            iconName = QStringLiteral("icon:lock");
        }

        QString rowClass = QStringLiteral("channel-row");
        if (isSelected) rowClass += QStringLiteral(" selected");
        if (isUnread) rowClass += QStringLiteral(" unread");
        if (isMuted) rowClass += QStringLiteral(" muted");

        rml += QStringLiteral("<div class=\"%1\" data-channel-id=\"%2\" data-type=\"%3\">")
                   .arg(rowClass)
                   .arg(node->id.toString())
                   .arg(isForum ? QStringLiteral("forum") : QStringLiteral("text"));

        if (isUnread)
            rml += QStringLiteral("<div class=\"channel-pill\"></div>");

        rml += QStringLiteral("<img class=\"channel-icon\" src=\"%1\"/>").arg(iconName);
        rml += QStringLiteral("<span class=\"channel-name\">%1</span>").arg(Rml::escape(node->name));

        if (mentions > 0) {
            QString badgeText = mentions > 99 ? QStringLiteral("99+") : QString::number(mentions);
            rml += QStringLiteral("<div class=\"channel-mention-badge\">%1</div>").arg(badgeText);
        }

        rml += QStringLiteral("</div>"); // .channel-row

        // Threads under channel
        const int threadCount = filter->rowCount(proxyIndex);
        for (int t = 0; t < threadCount; ++t) {
            renderChannelNode(filter->index(t, 0, proxyIndex), rml, depth + 1);
        }
        return;
    }

    if (node->type == ChannelNode::Type::VoiceChannel) {
        const bool isSelected = (node->id == shell.location().channelId);
        const bool isUnread = node->isUnread;
        const bool isMuted = node->isMuted;
        const int mentions = node->mentionCount;

        QString rowClass = QStringLiteral("channel-row");
        if (isSelected) rowClass += QStringLiteral(" selected");
        if (isUnread) rowClass += QStringLiteral(" unread");
        if (isMuted) rowClass += QStringLiteral(" muted");

        rml += QStringLiteral("<div class=\"%1\" data-channel-id=\"%2\" data-type=\"voice\">")
                   .arg(rowClass)
                   .arg(node->id.toString());

        if (isUnread)
            rml += QStringLiteral("<div class=\"channel-pill\"></div>");

        rml += QStringLiteral("<img class=\"channel-icon\" src=\"icon:volume-2\"/>");
        rml += QStringLiteral("<span class=\"channel-name\">%1</span>").arg(Rml::escape(node->name));

        if (mentions > 0) {
            QString badgeText = mentions > 99 ? QStringLiteral("99+") : QString::number(mentions);
            rml += QStringLiteral("<div class=\"channel-mention-badge\">%1</div>").arg(badgeText);
        }

        rml += QStringLiteral("</div>"); // .channel-row

        // Voice participants under voice channel
        const int partCount = filter->rowCount(proxyIndex);
        for (int p = 0; p < partCount; ++p) {
            renderChannelNode(filter->index(p, 0, proxyIndex), rml, depth + 1);
        }
        return;
    }

    if (node->type == ChannelNode::Type::Thread) {
        const bool isSelected = (node->id == shell.location().channelId);
        const bool isUnread = node->isUnread;
        const bool isMuted = node->isMuted;
        const int mentions = node->mentionCount;

        QString rowClass = QStringLiteral("channel-row thread");
        if (isSelected) rowClass += QStringLiteral(" selected");
        if (isUnread) rowClass += QStringLiteral(" unread");
        if (isMuted) rowClass += QStringLiteral(" muted");

        rml += QStringLiteral("<div class=\"%1\" data-channel-id=\"%2\" data-type=\"text\">")
                   .arg(rowClass)
                   .arg(node->id.toString());

        if (isUnread)
            rml += QStringLiteral("<div class=\"channel-pill\"></div>");

        rml += QStringLiteral("<img class=\"channel-icon\" src=\"icon:message-circle\"/>");
        rml += QStringLiteral("<span class=\"channel-name\">%1</span>").arg(Rml::escape(node->name));

        if (mentions > 0) {
            QString badgeText = mentions > 99 ? QStringLiteral("99+") : QString::number(mentions);
            rml += QStringLiteral("<div class=\"channel-mention-badge\">%1</div>").arg(badgeText);
        }

        rml += QStringLiteral("</div>"); // .channel-row.thread
        return;
    }

    if (node->type == ChannelNode::Type::VoiceParticipant) {
        const Snowflake userId = node->id;
        bool isSpeaking = false;
        bool isStreaming = false;
        bool isMuted = proxyIndex.data(ChannelTreeModel::IsVoiceMutedRole).toBool();
        bool isDeafened = proxyIndex.data(ChannelTreeModel::IsVoiceDeafenedRole).toBool();

#ifndef ACHERON_NO_VOICE
        if (auto *inst = shell.currentInstance()) {
            if (auto *vm = inst->voice()) {
                if (const auto *p = vm->participant(userId)) {
                    isSpeaking = p->speaking;
                    isStreaming = p->streaming;
                    isMuted = isMuted || p->selfMute || p->serverMute;
                    isDeafened = isDeafened || p->selfDeaf || p->serverDeaf;
                }
            }
        }
#endif

        QString avSrc;
        if (!node->voiceParticipantAvatarUrl.isEmpty()) {
            avSrc = shell.imageSource(node->voiceParticipantAvatarUrl, QSize(24, 24));
        } else {
            QUrl defUrl = Discord::Cdn::defaultUserAvatar(userId);
            avSrc = shell.imageSource(defUrl, QSize(24, 24));
        }

        QString rowClass = QStringLiteral("voice-participant-row");
        if (isSpeaking)
            rowClass += QStringLiteral(" speaking");

        rml += QStringLiteral("<div class=\"%1\" data-user-id=\"%2\">")
                   .arg(rowClass)
                   .arg(userId.toString());
        rml += QStringLiteral("<img class=\"vp-avatar\" src=\"%1\"/>").arg(Rml::escape(avSrc));
        rml += QStringLiteral("<span class=\"vp-name\">%1</span>").arg(Rml::escape(node->name));

        if (isStreaming) {
            rml += QStringLiteral("<span class=\"vp-badge-live\">LIVE</span>");
        }

        if (isDeafened) {
            rml += QStringLiteral("<img class=\"vp-status-icon\" src=\"icon:volume-x\"/>");
        } else if (isMuted) {
            rml += QStringLiteral("<img class=\"vp-status-icon\" src=\"icon:mic-off\"/>");
        }

        rml += QStringLiteral("</div>"); // .voice-participant-row
        return;
    }
}

} // namespace Rml
} // namespace UI
} // namespace Acheron
