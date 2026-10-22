#include "RailController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/Session.hpp"
#include "Discord/CdnUrls.hpp"
#include "UI/ChannelList/ChannelNode.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"
#include "UI/ChannelList/ServerRailModel.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

#include <QColor>
#include <QModelIndex>
#include <QRegularExpression>

namespace Acheron {
namespace UI {
namespace Rml {

using Core::Snowflake;

RailController::RailController(Shell &shell, ::Rml::Element *root, QObject *parent)
    : Controller(shell, root, parent)
{
    rebuildTimer.setSingleShot(true);
    rebuildTimer.setInterval(0);
    connect(&rebuildTimer, &QTimer::timeout, this, &RailController::rebuild);

    // Event listeners
    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        auto *item = closest(target, "data-kind");
        if (!item)
            return;

        QString kind = attribute(item, "data-kind");
        Snowflake accountId(attribute(item, "data-account-id").toULongLong());
        Snowflake id(attribute(item, "data-id").toULongLong());

        if (kind == QLatin1String("home")) {
            this->shell.openHome(accountId);
        } else if (kind == QLatin1String("folder")) {
            if (auto *rail = this->shell.rail())
                rail->toggleFolder(accountId, id);
        } else if (kind == QLatin1String("server")) {
            this->shell.openGuild(accountId, id);
        }
    });

    listen("mousedown", [this](::Rml::Event &event) {
        if (event.GetParameter<int>("button", 0) != 1) // Right click
            return;

        auto *target = event.GetTargetElement();
        auto *item = closest(target, "data-kind");
        if (!item)
            return;

        QString kind = attribute(item, "data-kind");
        Snowflake accountId(attribute(item, "data-account-id").toULongLong());
        Snowflake id(attribute(item, "data-id").toULongLong());

        if (kind == QLatin1String("server")) {
            this->shell.showContextMenuForGuild(accountId, id);
        } else if (kind == QLatin1String("home")) {
            this->shell.openAccounts();
        }
    });

    // Model and navigation signals
    connect(&shell, &Shell::locationChanged, this, [this](const Location &loc) {
        if (auto *rail = this->shell.rail()) {
            if (loc.isHome())
                rail->setSelected(ServerRailModel::Kind::AccountHome, loc.accountId, loc.accountId);
            else
                rail->setSelected(ServerRailModel::Kind::Server, loc.accountId, loc.guildId);
        }
        rebuildTimer.start();
    });

    connect(&shell, &Shell::instanceChanged, this, [this]() {
        rebuildTimer.start();
    });

    connect(&shell, &Shell::accountReady, this, [this]() {
        rebuildTimer.start();
    });

    if (auto *rail = shell.rail()) {
        connect(rail, &QAbstractItemModel::modelReset, this, [this]() { rebuildTimer.start(); });
        connect(rail, &QAbstractItemModel::dataChanged, this, [this]() { rebuildTimer.start(); });
        connect(rail, &QAbstractItemModel::rowsInserted, this, [this]() { rebuildTimer.start(); });
        connect(rail, &QAbstractItemModel::rowsRemoved, this, [this]() { rebuildTimer.start(); });
        connect(rail, &QAbstractItemModel::layoutChanged, this, [this]() { rebuildTimer.start(); });
    }

    renderRail();
}

void RailController::rebuild()
{
    renderRail();
}

QString RailController::acronym(const QString &name)
{
    const QStringList words = name.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QString res;
    for (const QString &w : words) {
        if (!w.isEmpty()) {
            res += w[0].toUpper();
            if (res.length() >= 4)
                break;
        }
    }
    if (res.isEmpty() && !name.isEmpty())
        res = name.left(1).toUpper();
    return res;
}

QString RailController::renderFolderGrid(Snowflake accountId, Snowflake folderId)
{
    QString rml;
    auto *tree = shell.channelTree();
    if (!tree)
        return rml;

    ChannelNode *folderNode = tree->findChannelTreeNode(folderId, accountId);
    if (!folderNode)
        return rml;

    int count = 0;
    for (const auto &child : folderNode->children) {
        if (child->type != ChannelNode::Type::Server)
            continue;
        if (count >= 4)
            break;

        if (!child->TEMP_iconHash.isEmpty()) {
            QUrl url = Discord::Cdn::guildIcon(child->id, child->TEMP_iconHash, 32);
            QString src = shell.imageSource(url, QSize(20, 20));
            rml += QStringLiteral("<div class=\"folder-grid-item\"><img src=\"%1\"/></div>")
                       .arg(Rml::escape(src));
        } else {
            QString acr = acronym(child->name);
            rml += QStringLiteral("<div class=\"folder-grid-item\"><span class=\"folder-grid-acr\">%1</span></div>")
                       .arg(Rml::escape(acr.left(2)));
        }
        count++;
    }
    return rml;
}

void RailController::renderRail()
{
    if (!regionRoot)
        return;

    auto *model = shell.rail();
    if (!model)
        return;

    QString rmlPrimary;
    QString rmlExpanded;
    Snowflake currentExpandedFolderId = Snowflake::Invalid;
    Snowflake lastExpandedFolderId = Snowflake::Invalid;

    const int rows = model->rowCount();
    for (int r = 0; r < rows; ++r) {
        QModelIndex idx = model->index(r, 0);
        auto kind = static_cast<ServerRailModel::Kind>(idx.data(ServerRailModel::KindRole).toInt());
        Snowflake accountId(idx.data(ServerRailModel::AccountIdRole).toULongLong());
        Snowflake id(idx.data(ServerRailModel::IdRole).toULongLong());
        QString name = idx.data(Qt::DisplayRole).toString();
        bool isUnread = idx.data(ServerRailModel::IsUnreadRole).toBool();
        int mentionCount = idx.data(ServerRailModel::MentionCountRole).toInt();
        bool isExpanded = idx.data(ServerRailModel::IsExpandedRole).toBool();
        bool isSelected = idx.data(ServerRailModel::IsSelectedRole).toBool();
        bool isFirstOfAccount = idx.data(ServerRailModel::IsFirstOfAccountRole).toBool();
        bool inFolderGroup = idx.data(ServerRailModel::InFolderGroupRole).toBool();
        if (kind == ServerRailModel::Kind::AccountHome) {

            QString itemClass = QStringLiteral("rail-item kind-home");
            if (isSelected)
                itemClass += QStringLiteral(" selected");
            if (isUnread)
                itemClass += QStringLiteral(" unread");

            QString itemRml;
            itemRml += QStringLiteral("<div class=\"%1\" data-kind=\"home\" data-account-id=\"%2\" data-id=\"%2\">")
                           .arg(itemClass)
                           .arg(accountId.toString());
            itemRml += QStringLiteral("<div class=\"pill\"></div>");
            itemRml += QStringLiteral("<div class=\"rail-btn\">");

            itemRml += QStringLiteral("<img class=\"rail-icon\" src=\"icon:messages-square\"/>");

            if (mentionCount > 0) {
                QString badgeText = mentionCount > 99 ? QStringLiteral("99+") : QString::number(mentionCount);
                itemRml += QStringLiteral("<div class=\"mention-badge\">%1</div>").arg(badgeText);
            }

            itemRml += QStringLiteral("</div>"); // .rail-btn
            itemRml += QStringLiteral("<div class=\"rail-tooltip\">%1</div>")
                           .arg(Rml::escape(name.isEmpty() ? QStringLiteral("Direct Messages") : name));
            itemRml += QStringLiteral("</div>"); // .rail-item

            if (isFirstOfAccount && r > 0) {
                rmlPrimary += QStringLiteral("<div class=\"rail-separator account-separator\"></div>");
            }
            rmlPrimary += itemRml;
            rmlPrimary += QStringLiteral("<div class=\"rail-separator\"></div>");
        } else if (kind == ServerRailModel::Kind::Folder) {
            QString itemClass = QStringLiteral("rail-item kind-folder");
            if (isSelected)
                itemClass += QStringLiteral(" selected");
            if (isUnread)
                itemClass += QStringLiteral(" unread");
            if (isExpanded)
                itemClass += QStringLiteral(" expanded");
            if (inFolderGroup)
                itemClass += QStringLiteral(" in-folder-group");

            if (isExpanded) {
                currentExpandedFolderId = id;
            }

            QVariant fcVar = idx.data(ServerRailModel::FolderColorRole);
            QString bgStyle;
            if (fcVar.isValid()) {
                QColor c = QColor::fromRgb(fcVar.toULongLong());
                bgStyle = QStringLiteral(" style=\"background-color: rgba(%1,%2,%3,0.3);\"")
                              .arg(c.red())
                              .arg(c.green())
                              .arg(c.blue());
            }

            QString itemRml;
            itemRml += QStringLiteral("<div class=\"%1\" data-kind=\"folder\" data-account-id=\"%2\" data-id=\"%3\">")
                           .arg(itemClass)
                           .arg(accountId.toString())
                           .arg(id.toString());
            itemRml += QStringLiteral("<div class=\"pill\"></div>");
            itemRml += QStringLiteral("<div class=\"rail-btn folder-btn\"%1>").arg(bgStyle);

            if (isExpanded) {
                itemRml += QStringLiteral("<img class=\"rail-icon\" src=\"icon:chevron-down\"/>");
            } else {
                itemRml += QStringLiteral("<div class=\"folder-grid\">");
                itemRml += renderFolderGrid(accountId, id);
                itemRml += QStringLiteral("</div>");
            }

            if (mentionCount > 0) {
                QString badgeText = mentionCount > 99 ? QStringLiteral("99+") : QString::number(mentionCount);
                itemRml += QStringLiteral("<div class=\"mention-badge\">%1</div>").arg(badgeText);
            }

            itemRml += QStringLiteral("</div>"); // .rail-btn
            itemRml += QStringLiteral("<div class=\"rail-tooltip\">%1</div>")
                           .arg(Rml::escape(name.isEmpty() ? QStringLiteral("Folder") : name));
            itemRml += QStringLiteral("</div>"); // .rail-item

            if (isFirstOfAccount && r > 0) {
                rmlPrimary += QStringLiteral("<div class=\"rail-separator account-separator\"></div>");
            }
            rmlPrimary += itemRml;
        } else if (kind == ServerRailModel::Kind::Server) {
            QString itemClass = QStringLiteral("rail-item kind-server");
            if (isSelected)
                itemClass += QStringLiteral(" selected");
            if (isUnread)
                itemClass += QStringLiteral(" unread");
            if (inFolderGroup)
                itemClass += QStringLiteral(" in-folder-group");

            QString iconHash;
            if (auto *tree = shell.channelTree())
                iconHash = tree->serverIndex(accountId, id).data(ChannelTreeModel::IconHashRole).toString();

            QString itemRml;
            itemRml += QStringLiteral("<div class=\"%1\" data-kind=\"server\" data-account-id=\"%2\" data-id=\"%3\">")
                           .arg(itemClass)
                           .arg(accountId.toString())
                           .arg(id.toString());
            itemRml += QStringLiteral("<div class=\"pill\"></div>");
            itemRml += QStringLiteral("<div class=\"rail-btn\">");

            if (!iconHash.isEmpty()) {
                QUrl url = Discord::Cdn::guildIcon(id, iconHash, 64);
                QString src = shell.imageSource(url, QSize(48, 48));
                itemRml += QStringLiteral("<img class=\"rail-avatar\" src=\"%1\"/>").arg(Rml::escape(src));
            } else {
                QString acr = acronym(name);
                itemRml += QStringLiteral("<span class=\"rail-monogram\">%1</span>").arg(Rml::escape(acr));
            }

            if (mentionCount > 0) {
                QString badgeText = mentionCount > 99 ? QStringLiteral("99+") : QString::number(mentionCount);
                itemRml += QStringLiteral("<div class=\"mention-badge\">%1</div>").arg(badgeText);
            }

            itemRml += QStringLiteral("</div>"); // .rail-btn
            itemRml += QStringLiteral("<div class=\"rail-tooltip\">%1</div>").arg(Rml::escape(name));
            itemRml += QStringLiteral("</div>"); // .rail-item

            if (!inFolderGroup) {
                if (isFirstOfAccount && r > 0) {
                    rmlPrimary += QStringLiteral("<div class=\"rail-separator account-separator\"></div>");
                }
                rmlPrimary += itemRml;
            } else {
                if (lastExpandedFolderId != currentExpandedFolderId) {
                    if (!rmlExpanded.isEmpty()) {
                        rmlExpanded += QStringLiteral("<div class=\"rail-separator\"></div>");
                    }
                    lastExpandedFolderId = currentExpandedFolderId;
                }
                rmlExpanded += itemRml;
            }
        }
    }

    const bool hasExpandedServers = !rmlExpanded.isEmpty();
    if (auto *doc = shell.document()) {
        doc->SetClass("rail-folder-open", hasExpandedServers);
    }

    QString rml;
    rml += QStringLiteral("<div class=\"rail-primary\">") + rmlPrimary + QStringLiteral("</div>");
    if (hasExpandedServers) {
        rml += QStringLiteral("<div class=\"rail-expanded\">") + rmlExpanded + QStringLiteral("</div>");
    }
    regionRoot->SetInnerRML(rml.toUtf8().constData());
    requestRender(200);
}

} // namespace Rml
} // namespace UI
} // namespace Acheron
