#include "MemberListController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"

#include "Core/Presence/ActivityFormat.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/MemberListManager.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"
#include "Discord/Entities.hpp"
#include "UI/MemberList/MemberListModel.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>

namespace Styx {
namespace UI {
namespace Rml {

using Core::Snowflake;

MemberListController::MemberListController(Shell &shell, ::Rml::Element *root, QObject *parent)
    : Controller(shell, root, parent)
{
    updateDebounceTimer.setSingleShot(true);
    updateDebounceTimer.setInterval(10);
    connect(&updateDebounceTimer, &QTimer::timeout, this, &MemberListController::updateMemberList);

    setupConnections();

    // Listen to scroll on root (#members)
    listen("scroll", [this](::Rml::Event &) { onScroll(); });

    // Listen to click and contextmenu on member items
    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        auto *item = closest(target, "data-user-id");
        if (!item)
            return;
        QString idStr = attribute(item, "data-user-id");
        if (idStr.isEmpty())
            return;
        Snowflake userId(idStr.toULongLong());
        if (userId.isValid()) {
            QRectF anchorRect(item->GetAbsoluteLeft(), item->GetAbsoluteTop(), item->GetClientWidth(), item->GetClientHeight());
            this->shell.showUserProfile(userId, activeGuildId, anchorRect);
        }
    });

    listen("contextmenu", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        auto *item = closest(target, "data-user-id");
        if (!item)
            return;
        QString idStr = attribute(item, "data-user-id");
        if (idStr.isEmpty())
            return;
        Snowflake userId(idStr.toULongLong());
        if (userId.isValid()) {
            this->shell.showContextMenuForUser(userId, activeGuildId);
        }
    });

    onInstanceChanged();
    onLocationChanged();
}

MemberListController::~MemberListController()
{
    disconnectCurrentManager();
}

void MemberListController::setupConnections()
{
    connect(&shell, &Shell::locationChanged, this, &MemberListController::onLocationChanged);
    connect(&shell, &Shell::instanceChanged, this, &MemberListController::onInstanceChanged);
    connect(&shell, &Shell::memberListVisibleChanged, this, [this](bool visible) {
        if (visible)
            updateDebounceTimer.start();
    });

    // Also observe the shell's MemberListModel signals as an additional trigger
    if (auto *model = shell.members()) {
        connect(model, &QAbstractItemModel::modelReset, this, [this]() {
            updateDebounceTimer.start();
        });
        connect(model, &QAbstractItemModel::dataChanged, this, [this]() {
            updateDebounceTimer.start();
        });
        connect(model, &QAbstractItemModel::rowsInserted, this, [this]() {
            updateDebounceTimer.start();
        });
        connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() {
            updateDebounceTimer.start();
        });
    }
}

void MemberListController::onInstanceChanged()
{
    disconnectCurrentManager();

    currentInstance = shell.currentInstance();
    if (currentInstance) {
        currentMemberListManager = currentInstance->memberList();
        currentPresenceManager = currentInstance->presences();
        connectCurrentManager();
    } else {
        currentMemberListManager = nullptr;
        currentPresenceManager = nullptr;
    }

    lastSubStart = -1;
    lastSubEnd = -1;
    updateDebounceTimer.start();
}

void MemberListController::connectCurrentManager()
{
    if (currentMemberListManager) {
        connect(currentMemberListManager, &Core::MemberListManager::listReset,
                this, [this]() { updateDebounceTimer.start(); });
        connect(currentMemberListManager, &Core::MemberListManager::itemsChanged,
                this, [this]() { updateDebounceTimer.start(); });
    }
    if (currentPresenceManager) {
        connect(currentPresenceManager, &Core::PresenceManager::presencesChanged,
                this, [this]() { updateDebounceTimer.start(); });
    }
}

void MemberListController::disconnectCurrentManager()
{
    if (currentMemberListManager)
        currentMemberListManager->disconnect(this);
    if (currentPresenceManager)
        currentPresenceManager->disconnect(this);
}

void MemberListController::onLocationChanged()
{
    const auto &loc = shell.location();
    activeGuildId = loc.guildId;
    activeChannelId = loc.channelId;

    lastSubStart = -1;
    lastSubEnd = -1;

    if (regionRoot)
        regionRoot->SetScrollTop(0);

    updateDebounceTimer.start();
}

void MemberListController::onScroll()
{
    if (!regionRoot || !activeGuildId.isValid() || !currentMemberListManager)
        return;

    const float scrollTop = regionRoot->GetScrollTop();
    const float clientHeight = regionRoot->GetClientHeight();

    const int totalCount = currentMemberListManager->totalItemCount();
    if (totalCount <= 0)
        return;

    int firstVisible = static_cast<int>(scrollTop / RowHeight);
    int visibleCount = static_cast<int>(std::ceil(clientHeight / RowHeight)) + 1;
    int lastVisible = firstVisible + visibleCount;

    // Expand for overscan
    int subStart = std::max(0, firstVisible - 10);
    int subEnd = std::min(totalCount - 1, lastVisible + 15);

    if (subStart != lastSubStart || subEnd != lastSubEnd) {
        lastSubStart = subStart;
        lastSubEnd = subEnd;
        currentMemberListManager->updateSubscriptionRange(subStart, subEnd);
        updateDebounceTimer.start();
    }
}

void MemberListController::updateMemberList()
{
    if (!regionRoot)
        return;

    if (!shell.isMemberListVisible()) {
        regionRoot->SetInnerRML("");
        requestRender();
        return;
    }

    if (activeGuildId.isValid()) {
        renderGuildMemberList();
    } else {
        // DM or Group DM
        if (!currentInstance || !activeChannelId.isValid()) {
            regionRoot->SetInnerRML("");
            requestRender();
            return;
        }

        auto chOpt = currentInstance->getChannel(activeChannelId);
        if (!chOpt) {
            regionRoot->SetInnerRML("");
            requestRender();
            return;
        }

        const auto &ch = *chOpt;
        QList<Snowflake> recipients;
        if (ch.recipients.hasValue()) {
            for (const auto &u : ch.recipients.get()) {
                if (u.id.hasValue() && u.id.get().isValid())
                    recipients.append(u.id.get());
            }
        } else if (ch.recipientIds.hasValue()) {
            recipients = ch.recipientIds.get();
        }

        if (recipients.size() > 1 || ch.type.valueOr(Discord::ChannelType::DM) == Discord::ChannelType::GROUP_DM) {
            renderGroupDmList(recipients);
        } else if (recipients.size() == 1) {
            renderSingleDmProfile(recipients.first());
        } else {
            regionRoot->SetInnerRML("");
            requestRender();
        }
    }
}

void MemberListController::renderGuildMemberList()
{
    if (!regionRoot || !currentMemberListManager) {
        if (regionRoot)
            regionRoot->SetInnerRML("");
        requestRender();
        return;
    }

    const int totalCount = currentMemberListManager->totalItemCount();
    if (totalCount == 0) {
        regionRoot->SetInnerRML("");
        requestRender();
        return;
    }

    // Determine guild owner
    Snowflake ownerId;
    if (currentInstance) {
        if (auto g = currentInstance->getGuild(activeGuildId)) {
            ownerId = g->ownerId.get();
        }
    }

    const float scrollTop = regionRoot->GetScrollTop();
    const float clientHeight = regionRoot->GetClientHeight() > 0 ? regionRoot->GetClientHeight() : 800.0f;

    // Subscribe visible range
    int firstVisible = static_cast<int>(scrollTop / RowHeight);
    int visibleCount = static_cast<int>(std::ceil(clientHeight / RowHeight)) + 1;
    int renderStart = std::max(0, firstVisible - 10);
    int renderEnd = std::min(totalCount - 1, firstVisible + visibleCount + 10);

    if (renderStart != lastSubStart || renderEnd != lastSubEnd) {
        lastSubStart = renderStart;
        lastSubEnd = renderEnd;
        currentMemberListManager->updateSubscriptionRange(renderStart, renderEnd);
    }

    const int totalHeight = totalCount * RowHeight;

    QString rml;
    rml.reserve(totalCount * 64);
    rml += QStringLiteral("<div class=\"members-scroll-area\" style=\"height: %1dp;\">").arg(totalHeight);

    for (int i = renderStart; i <= renderEnd; ++i) {
        const auto *item = currentMemberListManager->itemAt(i);
        const int topPos = i * RowHeight;

        if (!item || item->type == Core::MemberListItem::Type::Placeholder) {
            // Render placeholder skeleton
            rml += QStringLiteral(
                "<div class=\"member-item member-row\" style=\"top: %1dp;\">"
                "  <div class=\"member-avatar-wrap\"><div class=\"member-avatar-fallback\"></div></div>"
                "  <div class=\"member-content\"><div class=\"member-name\" style=\"opacity: 0.3;\">...</div></div>"
                "</div>"
            ).arg(topPos);
            continue;
        }

        if (item->type == Core::MemberListItem::Type::Group) {
            QString groupTitle = escape(item->groupName.toUpper()) + QStringLiteral(" — ") + QString::number(item->groupCount);
            rml += QStringLiteral(
                "<div class=\"member-item member-group-header\" style=\"top: %1dp;\">%2</div>"
            ).arg(topPos).arg(groupTitle);
            continue;
        }

        if (item->type == Core::MemberListItem::Type::Member) {
            const Snowflake userId = item->userId;
            const QString displayName = escape(item->displayName);

            // Role colour
            QString nameStyle;
            if (item->roleColor.isValid()) {
                nameStyle = QStringLiteral("color: %1;").arg(item->roleColor.name());
            }

            // Avatar URL
            QUrl avatarUrl;
            QString userAvatarHash;
            if (item->member.user.hasValue() && item->member.user->avatar.hasValue())
                userAvatarHash = item->member.user->avatar.get();
            if (item->member.avatar.hasValue() && !item->member.avatar->isEmpty()) {
                avatarUrl = Discord::Cdn::effectiveAvatar(activeGuildId, userId, item->member.avatar.valueOr(),
                                                         userAvatarHash, 64);
            } else if (!userAvatarHash.isEmpty()) {
                avatarUrl = Discord::Cdn::userAvatar(userId, userAvatarHash, 64);
            } else {
                avatarUrl = Discord::Cdn::defaultUserAvatar(userId, 64);
            }
            QString avatarSrc = shell.imageSource(avatarUrl, QSize(32, 32));

            // Presence
            Core::PresenceBadge badge;
            if (currentPresenceManager)
                badge = currentPresenceManager->badge(userId, activeGuildId);

            QString statusClass;
            bool isOffline = false;
            switch (badge.status) {
            case Discord::StatusType::ONLINE:
                statusClass = QStringLiteral("status-online");
                break;
            case Discord::StatusType::IDLE:
                statusClass = QStringLiteral("status-idle");
                break;
            case Discord::StatusType::DND:
                statusClass = QStringLiteral("status-dnd");
                break;
            case Discord::StatusType::OFFLINE:
            default:
                statusClass = QStringLiteral("status-offline");
                isOffline = true;
                break;
            }
            if (badge.streaming)
                statusClass = QStringLiteral("status-streaming");

            // Activity text
            QString activityHtml;
            if (currentPresenceManager) {
                if (const auto *act = currentPresenceManager->primaryActivity(userId, activeGuildId)) {
                    QString actText = Core::ActivityFormat::secondaryText(*act);
                    if (!actText.isEmpty()) {
                        activityHtml = QStringLiteral("<div class=\"member-activity\">%1</div>").arg(richText(actText));
                    }
                }
            }

            // Owner crown
            QString crownHtml;
            if (ownerId.isValid() && userId == ownerId) {
                crownHtml = QStringLiteral("<img class=\"member-crown\" src=\"icon:crown\"/>");
            }

            rml += QStringLiteral(
                "<div class=\"member-item member-row %1\" style=\"top: %2dp;\" data-user-id=\"%3\">"
                "  <div class=\"member-avatar-wrap\">"
                "    <div class=\"member-avatar-clip\">"
                "      <img class=\"member-avatar\" src=\"%4\"/>"
                "    </div>"
                "    <div class=\"member-status-dot %5\"></div>"
                "  </div>"
                "  <div class=\"member-content\">"
                "    <div class=\"member-name-row\">"
                "      <div class=\"member-name\" style=\"%6\">%7</div>"
                "      %8"
                "    </div>"
                "    %9"
                "  </div>"
                "</div>"
            ).arg(isOffline ? QStringLiteral("offline") : QString(),
                 QString::number(topPos),
                 userId.toString(),
                 avatarSrc,
                 statusClass,
                 nameStyle,
                 displayName,
                 crownHtml,
                 activityHtml);
        }
    }

    rml += QStringLiteral("</div>");
    regionRoot->SetInnerRML(rml.toUtf8().constData());
    requestRender();
}

void MemberListController::renderGroupDmList(const QList<Snowflake> &recipients)
{
    if (!regionRoot || !currentInstance) {
        if (regionRoot)
            regionRoot->SetInnerRML("");
        requestRender();
        return;
    }

    auto *users = currentInstance->users();

    // Group DM list header
    QString rml;
    rml += QStringLiteral("<div class=\"member-group-header\">MEMBERS — %1</div>").arg(recipients.size());

    for (Snowflake uid : recipients) {
        QString name = users ? users->getDisplayName(uid) : uid.toString();
        if (name.isEmpty())
            name = uid.toString();
        QUrl avatarUrl;
        if (users) {
            if (auto userOpt = users->getUser(uid)) {
                if (userOpt->avatar.hasValue() && !userOpt->avatar->isEmpty())
                    avatarUrl = Discord::Cdn::userAvatar(uid, userOpt->avatar.get(), 64);
            }
        }
        if (!avatarUrl.isValid())
            avatarUrl = Discord::Cdn::defaultUserAvatar(uid, 64);

        QString avatarSrc = shell.imageSource(avatarUrl, QSize(32, 32));

        Core::PresenceBadge badge;
        if (currentPresenceManager)
            badge = currentPresenceManager->badge(uid);

        QString statusClass;
        bool isOffline = false;
        switch (badge.status) {
        case Discord::StatusType::ONLINE:
            statusClass = QStringLiteral("status-online");
            break;
        case Discord::StatusType::IDLE:
            statusClass = QStringLiteral("status-idle");
            break;
        case Discord::StatusType::DND:
            statusClass = QStringLiteral("status-dnd");
            break;
        case Discord::StatusType::OFFLINE:
        default:
            statusClass = QStringLiteral("status-offline");
            isOffline = true;
            break;
        }
        if (badge.streaming)
            statusClass = QStringLiteral("status-streaming");

        QString activityHtml;
        if (currentPresenceManager) {
            if (const auto *act = currentPresenceManager->primaryActivity(uid)) {
                QString actText = Core::ActivityFormat::secondaryText(*act);
                if (!actText.isEmpty()) {
                    activityHtml = QStringLiteral("<div class=\"member-activity\">%1</div>").arg(richText(actText));
                }
            }
        }

        rml += QStringLiteral(
            "<div class=\"member-row %1\" data-user-id=\"%2\" style=\"position: relative; margin-bottom: 2dp;\">"
            "  <div class=\"member-avatar-wrap\">"
            "    <div class=\"member-avatar-clip\">"
            "      <img class=\"member-avatar\" src=\"%3\"/>"
            "    </div>"
            "    <div class=\"member-status-dot %4\"></div>"
            "  </div>"
            "  <div class=\"member-content\">"
            "    <div class=\"member-name-row\"><div class=\"member-name\">%5</div></div>"
            "    %6"
            "  </div>"
            "</div>"
        ).arg(isOffline ? QStringLiteral("offline") : QString(),
             uid.toString(),
             avatarSrc,
             statusClass,
             escape(name),
             activityHtml);
    }

    regionRoot->SetInnerRML(rml.toUtf8().constData());
    requestRender();
}

void MemberListController::renderSingleDmProfile(Snowflake targetUserId)
{
    if (!regionRoot || !currentInstance) {
        if (regionRoot)
            regionRoot->SetInnerRML("");
        requestRender();
        return;
    }

    auto *users = currentInstance->users();
    auto userOpt = users ? users->getUser(targetUserId) : std::nullopt;

    QString displayName;
    QString username;
    QUrl avatarUrl;
    QString bannerColor = QStringLiteral("#5865f2");

    if (userOpt) {
        displayName = userOpt->getDisplayName();
        username = userOpt->tag();
        if (userOpt->avatar.hasValue() && !userOpt->avatar->isEmpty()) {
            avatarUrl = Discord::Cdn::userAvatar(targetUserId, userOpt->avatar.get(), 128);
        }
        if (userOpt->accentColor.hasValue() && userOpt->accentColor.get() != 0) {
            bannerColor = QColor::fromRgb(userOpt->accentColor.get()).name();
        }
    } else {
        displayName = targetUserId.toString();
        username = displayName;
    }
    if (!avatarUrl.isValid())
        avatarUrl = Discord::Cdn::defaultUserAvatar(targetUserId, 128);

    QString avatarSrc = shell.imageSource(avatarUrl, QSize(80, 80));

    Core::PresenceBadge badge;
    if (currentPresenceManager)
        badge = currentPresenceManager->badge(targetUserId);

    QString statusClass = QStringLiteral("status-offline");
    switch (badge.status) {
    case Discord::StatusType::ONLINE:
        statusClass = QStringLiteral("status-online");
        break;
    case Discord::StatusType::IDLE:
        statusClass = QStringLiteral("status-idle");
        break;
    case Discord::StatusType::DND:
        statusClass = QStringLiteral("status-dnd");
        break;
    case Discord::StatusType::OFFLINE:
    default:
        statusClass = QStringLiteral("status-offline");
        break;
    }
    if (badge.streaming)
        statusClass = QStringLiteral("status-streaming");

    QString activityHtml;
    if (currentPresenceManager) {
        if (const auto *act = currentPresenceManager->primaryActivity(targetUserId)) {
            QString actText = Core::ActivityFormat::secondaryText(*act);
            if (!actText.isEmpty()) {
                activityHtml = QStringLiteral(
                    "<div class=\"dm-profile-divider\"></div>"
                    "<div class=\"dm-profile-section-title\">Activity</div>"
                    "<div class=\"dm-profile-activity\">%1</div>"
                ).arg(richText(actText));
            }
        }
    }

    QString rml = QStringLiteral(
        "<div class=\"dm-profile-panel\" data-user-id=\"%1\">"
        "  <div class=\"dm-profile-banner\" style=\"background-color: %2;\"></div>"
        "  <div class=\"dm-profile-body\">"
        "    <div class=\"dm-profile-avatar-wrap\">"
        "      <div class=\"dm-profile-avatar-clip\">"
        "        <img class=\"dm-profile-avatar\" src=\"%3\"/>"
        "      </div>"
        "      <div class=\"dm-profile-status-dot %4\"></div>"
        "    </div>"
        "    <div class=\"dm-profile-display-name\">%5</div>"
        "    <div class=\"dm-profile-username\">%6</div>"
        "    %7"
        "  </div>"
        "</div>"
    ).arg(targetUserId.toString())
     .arg(bannerColor)
     .arg(avatarSrc)
     .arg(statusClass)
     .arg(escape(displayName))
     .arg(escape(username))
     .arg(activityHtml);

    regionRoot->SetInnerRML(rml.toUtf8().constData());
    requestRender();
}

} // namespace Rml
} // namespace UI
} // namespace Styx
