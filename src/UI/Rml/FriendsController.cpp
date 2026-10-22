#include "FriendsController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/Presence/ActivityFormat.hpp"
#include "Core/Presence/PresenceBadge.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Core/RelationshipManager.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <QDateTime>
#include <utility>

namespace Styx {
namespace UI {
namespace Rml {

using Core::Snowflake;

FriendsController::FriendsController(Shell &shell, ::Rml::Element *root, QObject *parent)
    : Controller(shell, root, parent)
{
    rebuildTimer.setSingleShot(true);
    rebuildTimer.setInterval(0);
    connect(&rebuildTimer, &QTimer::timeout, this, &FriendsController::rebuild);

    connect(&shell, &Shell::locationChanged, this, [this](const Location &) {
        if (this->shell.isFriendsOpen()) {
            rebuildTimer.start();
        }
    });

    connect(&shell, &Shell::instanceChanged, this, [this](Core::ClientInstance *inst) {
        connectInstance(inst);
        if (this->shell.isFriendsOpen()) {
            rebuildTimer.start();
        }
    });

    connect(&shell, &Shell::accountReady, this, [this]() {
        if (this->shell.isFriendsOpen()) {
            rebuildTimer.start();
        }
    });

    setupEventListeners();
    connectInstance(shell.currentInstance());

    if (shell.isFriendsOpen()) {
        render();
    }
}

FriendsController::~FriendsController() = default;

void FriendsController::connectInstance(Core::ClientInstance *instance)
{
    if (lastInstance == instance)
        return;

    if (lastInstance) {
        disconnect(lastInstance, nullptr, this, nullptr);
        if (auto *rel = lastInstance->relationships())
            disconnect(rel, nullptr, this, nullptr);
        if (auto *prs = lastInstance->presences())
            disconnect(prs, nullptr, this, nullptr);
        if (auto *usr = lastInstance->users())
            disconnect(usr, nullptr, this, nullptr);
    }

    lastInstance = instance;
    ++requestGeneration;
    searchText.clear();
    addFriendInputText.clear();
    addFriendFeedback.clear();
    addFriendPending = false;
    addFriendSuccess = false;
    currentTab = Tab::Online;
    if (regionRoot)
        regionRoot->SetInnerRML("");
    if (!instance)
        return;

    if (auto *rel = instance->relationships()) {
        connect(rel, &Core::RelationshipManager::relationshipChanged, this, [this](Snowflake) {
            if (this->shell.isFriendsOpen())
                rebuildTimer.start();
        });
        connect(rel, &Core::RelationshipManager::relationshipsReset, this, [this]() {
            if (this->shell.isFriendsOpen())
                rebuildTimer.start();
        });
    }

    if (auto *prs = instance->presences()) {
        connect(prs, &Core::PresenceManager::presencesChanged, this, [this](const QList<Snowflake> &) {
            if (this->shell.isFriendsOpen())
                rebuildTimer.start();
        });
    }

    connect(instance, &Core::ClientInstance::membersUpdated, this, [this](Snowflake, const QList<Snowflake> &) {
        if (this->shell.isFriendsOpen())
            rebuildTimer.start();
    });
}

void FriendsController::setupEventListeners()
{
    if (!regionRoot)
        return;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();

        if (auto *tabEl = closest(target, "data-tab")) {
            QString tabStr = attribute(tabEl, "data-tab");
            if (tabStr == QLatin1String("online"))
                switchTab(Tab::Online);
            else if (tabStr == QLatin1String("all"))
                switchTab(Tab::All);
            else if (tabStr == QLatin1String("add"))
                switchTab(Tab::AddFriend);
            return;
        }

        if (auto *act = closest(target, "data-action")) {
            QString action = attribute(act, "data-action");
            if (action == QLatin1String("message")) {
                Snowflake uId(attribute(act, "data-user-id").toULongLong());
                if (uId.isValid())
                    this->shell.openDmWithUser(this->shell.location().accountId, uId);
                return;
            }
            if (action == QLatin1String("more")) {
                Snowflake uId(attribute(act, "data-user-id").toULongLong());
                if (uId.isValid())
                    this->shell.showContextMenuForUser(uId, Snowflake::Invalid);
                return;
            }
            if (action == QLatin1String("clear-friends-search")) {
                searchText.clear();
                if (auto *inp = dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("friends-search-input"))) {
                    inp->SetValue("");
                }
                rebuild();
                return;
            }
            if (action == QLatin1String("send-friend-request")) {
                sendFriendRequest();
                return;
            }
        }

        if (auto *row = closest(target, "data-user-id")) {
            Snowflake uId(attribute(row, "data-user-id").toULongLong());
            if (uId.isValid())
                this->shell.openDmWithUser(this->shell.location().accountId, uId);
            return;
        }
    });

    listen("mousedown", [this](::Rml::Event &event) {
        if (event.GetParameter<int>("button", 0) != 1) // Right click
            return;

        auto *target = event.GetTargetElement();
        if (auto *row = closest(target, "data-user-id")) {
            Snowflake uId(attribute(row, "data-user-id").toULongLong());
            if (uId.isValid())
                this->shell.showContextMenuForUser(uId, Snowflake::Invalid);
        }
    });

    listen("change", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target)) {
            if (input->GetId() == "friends-search-input") {
                QString val = QString::fromStdString(input->GetValue());
                if (val != searchText) {
                    searchText = val;
                    rebuildTimer.start();
                }
            } else if (input->GetId() == "add-friend-input") {
                addFriendInputText = QString::fromStdString(input->GetValue());
                updateAddFriendControls();
            }
        }
    });

    listen("keydown", [this](::Rml::Event &event) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(event.GetTargetElement())) {
            if (input->GetId() == "add-friend-input") {
                const int key = event.GetParameter<int>("key_identifier", 0);
                if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER) {
                    event.StopPropagation();
                    addFriendInputText = QString::fromStdString(input->GetValue());
                    sendFriendRequest();
                }
            }
        }
    }, nullptr, true);
}

void FriendsController::switchTab(Tab newTab)
{
    if (currentTab == newTab)
        return;
    currentTab = newTab;
    addFriendFeedback.clear();
    rebuild();
}

void FriendsController::sendFriendRequest()
{
    if (addFriendPending)
        return;

    QString tag = addFriendInputText.trimmed();
    if (tag.isEmpty())
        return;

    auto *inst = shell.currentInstance();
    if (!inst || !inst->discord()) {
        addFriendFeedback = QStringLiteral("Client instance not available.");
        addFriendSuccess = false;
        rebuildTimer.start();
        return;
    }

    addFriendPending = true;
    const quint64 generation = ++requestGeneration;
    const QPointer<FriendsController> self(this);
    inst->discord()->sendFriendRequest(tag, [self, generation, tag](const Core::Result<void> &res) {
        if (!self || self->requestGeneration != generation)
            return;
        self->addFriendPending = false;
        self->addFriendSuccess = res.success();
        if (res.success()) {
            self->addFriendFeedback = QStringLiteral("Friend request sent to %1.").arg(tag);
            self->addFriendInputText.clear();
        } else {
            self->addFriendFeedback = res.error;
        }
        self->rebuildTimer.start();
    });
    rebuildTimer.start();
}

void FriendsController::updateAddFriendControls()
{
    if (!regionRoot)
        return;
    if (auto *placeholder = regionRoot->GetElementById("add-friend-placeholder"))
        placeholder->SetProperty("display", addFriendInputText.isEmpty() ? "block" : "none");
    if (auto *button = regionRoot->GetElementById("add-friend-btn"))
        button->SetClass("disabled", addFriendPending || addFriendInputText.trimmed().isEmpty());
    requestRender();
}

void FriendsController::rebuild()
{
    if (!shell.isFriendsOpen())
        return;
    render();
    requestRender(200);
}

void FriendsController::render()
{
    if (!regionRoot)
        return;

    auto *inst = shell.currentInstance();
    int onlineCount = 0;
    int allCount = 0;

    if (inst && inst->relationships() && inst->presences()) {
        const auto &allRels = inst->relationships()->all();
        for (auto it = allRels.cbegin(); it != allRels.cend(); ++it) {
            if (!it.value().type.hasValue() || it.value().type.get() != Discord::RelationshipType::FRIEND)
                continue;
            ++allCount;
            auto badge = inst->presences()->badge(it.key());
            if (badge.status != Discord::StatusType::OFFLINE)
                ++onlineCount;
        }
    }

    auto *existingSearch = regionRoot->GetElementById("friends-search-input");
    auto *listContainer = regionRoot->GetElementById("friends-list-container");
    auto *activeNowList = regionRoot->GetElementById("active-now-list");
    auto *headerEl = regionRoot->GetElementById("friends-header");

    if (currentTab != Tab::AddFriend && existingSearch && listContainer && activeNowList && headerEl) {
        QString headerRml;
        renderHeader(headerRml, onlineCount, allCount);
        headerEl->SetInnerRML(headerRml.toUtf8().constData());

        if (auto *placeholder = regionRoot->GetElementById("friends-search-placeholder")) {
            placeholder->SetProperty("display", searchText.isEmpty() ? "inline" : "none");
        }
        if (auto *clear = regionRoot->GetElementById("friends-search-clear"))
            clear->SetProperty("display", searchText.isEmpty() ? "none" : "block");

        QString listRml;
        renderFriendsList(listRml, onlineCount, allCount);
        listContainer->SetInnerRML(listRml.toUtf8().constData());

        QString activeRml;
        renderActiveNow(activeRml);
        if (auto *activeNowEl = regionRoot->GetElementById("friends-active-now")) {
            activeNowEl->SetInnerRML(activeRml.toUtf8().constData());
        }
        return;
    }

    QString rml;
    rml += QStringLiteral("<div id=\"friends-header\">");
    renderHeader(rml, onlineCount, allCount);
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div id=\"friends-body\">");
    rml += QStringLiteral("<div id=\"friends-main\">");

    if (currentTab == Tab::AddFriend) {
        renderAddFriend(rml);
    } else {
        const QString clearBtn = QStringLiteral(
            "<img id=\"friends-search-clear\" class=\"friends-search-clear\" src=\"icon:x\" data-action=\"clear-friends-search\" style=\"%1\"/>")
            .arg(searchText.isEmpty() ? QStringLiteral("display: none;") : QString());

        rml += QStringLiteral(
            "<div class=\"friends-search-box\">"
            "<input type=\"text\" id=\"friends-search-input\" class=\"friends-search-input\" value=\"%1\"/>"
            "<span id=\"friends-search-placeholder\" class=\"friends-search-placeholder\" style=\"%2\">Search</span>"
            "<img class=\"friends-search-icon\" src=\"icon:search\"/>"
            "%3"
            "</div>")
            .arg(Rml::escape(searchText),
                 searchText.isEmpty() ? QString() : QStringLiteral("display: none;"),
                 clearBtn);

        rml += QStringLiteral("<div id=\"friends-list-container\">");
        renderFriendsList(rml, onlineCount, allCount);
        rml += QStringLiteral("</div>"); // #friends-list-container
    }

    rml += QStringLiteral("</div>"); // #friends-main

    rml += QStringLiteral("<div id=\"friends-active-now\">");
    renderActiveNow(rml);
    rml += QStringLiteral("</div>"); // #friends-active-now

    rml += QStringLiteral("</div>"); // #friends-body

    regionRoot->SetInnerRML(rml.toUtf8().constData());
}

void FriendsController::renderHeader(QString &rml, int onlineCount, int allCount)
{
    const QString onlineClass = (currentTab == Tab::Online) ? QStringLiteral(" selected") : QString();
    const QString allClass = (currentTab == Tab::All) ? QStringLiteral(" selected") : QString();
    const QString addClass = (currentTab == Tab::AddFriend) ? QStringLiteral(" selected") : QString();

    rml += QStringLiteral(
        "<div class=\"friends-header-title\">"
        "<img class=\"friends-header-icon\" src=\"icon:user-plus\"/>"
        "<span class=\"friends-header-text\">Friends</span>"
        "</div>"
        "<div class=\"friends-header-divider\"></div>"
        "<div class=\"friends-tab%1\" data-tab=\"online\">Online%2</div>"
        "<div class=\"friends-tab%3\" data-tab=\"all\">All%4</div>"
        "<div class=\"friends-tab-add%5\" data-tab=\"add\">Add Friend</div>")
        .arg(onlineClass)
        .arg(onlineCount > 0 ? QStringLiteral(" (%1)").arg(onlineCount) : QString())
        .arg(allClass)
        .arg(allCount > 0 ? QStringLiteral(" (%1)").arg(allCount) : QString())
        .arg(addClass);
}

void FriendsController::renderFriendsList(QString &rml, int onlineCount, int allCount)
{
    auto *inst = shell.currentInstance();
    if (!inst || !inst->relationships() || !inst->presences())
        return;
    const auto &allRels = inst->relationships()->all();

    struct FriendItem {
        Snowflake userId;
        QString displayName;
        QString username;
        QString avatarSrc;
        QString statusClass;
        QString subtext;
        bool isOnline = false;
    };

    QList<FriendItem> items;
    items.reserve(allRels.size());

    const QString searchFilter = searchText.trimmed().toLower();

    for (auto it = allRels.cbegin(); it != allRels.cend(); ++it) {
        const auto &rel = it.value();
        if (!rel.type.hasValue() || rel.type.get() != Discord::RelationshipType::FRIEND)
            continue;

        Snowflake userId = it.key();
        auto badge = inst->presences()->badge(userId);
        const bool isOnline = (badge.status != Discord::StatusType::OFFLINE);

        if (currentTab == Tab::Online && !isOnline)
            continue;

        auto userOpt = inst->users()->getUser(userId);
        Discord::User user;
        if (userOpt)
            user = *userOpt;
        else if (rel.user.hasValue())
            user = rel.user.get();

        QString displayName = inst->users()->getDisplayName(userId);
        if (displayName.isEmpty())
            displayName = user.globalName.valueOr(user.username.valueOr(QStringLiteral("Friend")));

        QString username = user.username.valueOr();

        if (!searchFilter.isEmpty()) {
            if (!displayName.toLower().contains(searchFilter) && !username.toLower().contains(searchFilter))
                continue;
        }

        QUrl avatarUrl = user.avatar.hasValue()
                             ? Discord::Cdn::userAvatar(userId, user.avatar.get(), 64)
                             : Discord::Cdn::defaultUserAvatar(userId);
        QString avatarSrc = shell.imageSource(avatarUrl, QSize(36, 36));

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

        auto acts = inst->presences()->activities(userId);
        auto sorted = Core::ActivityFormat::sortAndFilter(acts);
        const auto *custom = Core::ActivityFormat::custom(acts);
        const auto *prim = Core::ActivityFormat::primary(sorted);

        QString subtext;
        if (custom && !custom->stateText().isEmpty()) {
            subtext = custom->stateText();
        } else if (prim) {
            subtext = Core::ActivityFormat::cardHeading(*prim);
            QString sec = Core::ActivityFormat::secondaryText(*prim);
            if (!sec.isEmpty())
                subtext += QStringLiteral(": ") + sec;
        } else {
            switch (badge.status) {
            case Discord::StatusType::ONLINE: subtext = QStringLiteral("Online"); break;
            case Discord::StatusType::IDLE: subtext = QStringLiteral("Idle"); break;
            case Discord::StatusType::DND: subtext = QStringLiteral("Do Not Disturb"); break;
            case Discord::StatusType::OFFLINE:
            default: subtext = QStringLiteral("Offline"); break;
            }
        }

        items.append({ userId, displayName, username, avatarSrc, statusClass, subtext, isOnline });
    }

    std::sort(items.begin(), items.end(), [](const FriendItem &a, const FriendItem &b) {
        return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
    });

    const QString sectionTitle = (currentTab == Tab::Online)
                                     ? QStringLiteral("Online — %1").arg(items.size())
                                     : QStringLiteral("All friends — %1").arg(items.size());

    rml += QStringLiteral("<div class=\"friends-section-title\">%1</div>").arg(sectionTitle);
    rml += QStringLiteral("<div id=\"friends-list\">");

    if (items.isEmpty()) {
        QString emptyTitle;
        QString emptySubtitle;
        if (!searchText.isEmpty()) {
            emptyTitle = QStringLiteral("No friends found");
            emptySubtitle = QStringLiteral("No friends found matching \"%1\".").arg(Rml::escape(searchText));
        } else if (currentTab == Tab::Online) {
            emptyTitle = QStringLiteral("No one's around to play with Wumpus.");
            emptySubtitle = QStringLiteral("None of your friends are currently online.");
        } else {
            emptyTitle = QStringLiteral("You don't have any friends yet.");
            emptySubtitle = QStringLiteral("Add friends using their Discord Tag or username!");
        }

        rml += QStringLiteral(
            "<div class=\"friends-empty-state\">"
            "<img class=\"empty-state-icon\" src=\"icon:user-plus\"/>"
            "<div class=\"empty-state-title\">%1</div>"
            "<div class=\"empty-state-subtitle\">%2</div>"
            "</div>")
            .arg(emptyTitle)
            .arg(emptySubtitle);
    } else {
        for (const auto &item : items) {
            QString usernamePart;
            if (!item.username.isEmpty() && item.username != item.displayName) {
                usernamePart = QStringLiteral("<span class=\"friend-username\">@%1</span>").arg(Rml::escape(item.username));
            }

            rml += QStringLiteral(
                "<div class=\"friend-row\" data-user-id=\"%1\">"
                "<div class=\"friend-avatar-wrap\">"
                "<div class=\"avatar-clip\">"
                "<img class=\"friend-avatar\" src=\"%2\"/>"
                "</div>"
                "<div class=\"status-dot %3\"></div>"
                "</div>"
                "<div class=\"friend-info\">"
                "<div class=\"friend-name-line\">"
                "<span class=\"friend-display-name\">%4</span>"
                "%5"
                "</div>"
                "<div class=\"friend-subtext\">%6</div>"
                "</div>"
                "<div class=\"friend-actions\">"
                "<div class=\"friend-action-btn\" data-action=\"message\" data-user-id=\"%1\" title=\"Message\">"
                "<img class=\"icon\" src=\"icon:message-circle\"/>"
                "</div>"
                "<div class=\"friend-action-btn friend-action-more\" data-action=\"more\" data-user-id=\"%1\" title=\"More\">"
                "<img class=\"icon\" src=\"icon:ellipsis\"/>"
                "</div>"
                "</div>"
                "</div>")
                .arg(item.userId.toString(), escape(item.avatarSrc), item.statusClass,
                     escape(item.displayName), usernamePart, escape(item.subtext));
        }
    }

    rml += QStringLiteral("</div>"); // #friends-list
}

void FriendsController::renderActiveNow(QString &rml)
{
    auto *inst = shell.currentInstance();
    rml += QStringLiteral("<div class=\"active-now-title\">Active Now</div>");
    rml += QStringLiteral("<div id=\"active-now-list\">");

    struct CardData {
        Snowflake userId;
        QString displayName;
        QString avatarSrc;
        QString statusClass;
        QString typeIcon;
        QString activityType;
        QString heading;
        QString details;
        QString state;
        QString artworkSrc;
        bool streaming;
    };

    QList<CardData> cards;

    if (inst && inst->relationships() && inst->presences()) {
        const auto &allRels = inst->relationships()->all();
        for (auto it = allRels.cbegin(); it != allRels.cend(); ++it) {
            if (!it.value().type.hasValue() || it.value().type.get() != Discord::RelationshipType::FRIEND)
                continue;

            Snowflake userId = it.key();
            auto acts = inst->presences()->activities(userId);
            auto sorted = Core::ActivityFormat::sortAndFilter(acts);
            auto profileCards = Core::ActivityFormat::profileCardActivities(sorted);
            if (profileCards.isEmpty())
                continue;

            auto userOpt = inst->users()->getUser(userId);
            Discord::User user;
            if (userOpt)
                user = *userOpt;
            else if (it.value().user.hasValue())
                user = it.value().user.get();

            QString displayName = inst->users()->getDisplayName(userId);
            if (displayName.isEmpty())
                displayName = user.globalName.valueOr(user.username.valueOr(QStringLiteral("Friend")));

            QUrl avatarUrl = user.avatar.hasValue()
                                 ? Discord::Cdn::userAvatar(userId, user.avatar.get(), 32)
                                 : Discord::Cdn::defaultUserAvatar(userId);
            QString avatarSrc = shell.imageSource(avatarUrl, QSize(32, 32));

            auto badge = inst->presences()->badge(userId);
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

            for (const auto &act : profileCards) {
                QString typeIcon = QStringLiteral("icon:gamepad-2");
                QString typeText = QStringLiteral("Playing");

                if (act.kind() == Discord::ActivityType::LISTENING) {
                    typeIcon = QStringLiteral("icon:music");
                    typeText = QStringLiteral("Listening to %1").arg(act.name.valueOr(QStringLiteral("music")));
                } else if (act.kind() == Discord::ActivityType::STREAMING) {
                    typeIcon = QStringLiteral("icon:monitor");
                    typeText = QStringLiteral("Streaming");
                }

                QString heading = Core::ActivityFormat::cardHeading(act);
                if (heading.isEmpty())
                    heading = act.name.valueOr();

                QString details = act.detailsText();
                QString state = act.stateText();
                if (act.kind() == Discord::ActivityType::LISTENING && !details.isEmpty()) {
                    heading = std::move(details);
                    details = std::move(state);
                    state = act.assets.hasValue() ? act.assets->largeText.valueOr() : QString();
                    if (state == heading || state == details)
                        state.clear();
                }

                const bool streaming = act.kind() == Discord::ActivityType::STREAMING;
                const QSize artSize = streaming ? QSize(320, 180) : QSize(64, 64);
                QString artworkSrc;
                if (act.assets.hasValue() && act.assets->largeImage.hasValue()) {
                    const QUrl artwork = Discord::Cdn::activityAsset(
                        act.applicationId.valueOr(Snowflake::Invalid), *act.assets->largeImage, streaming ? 512 : 64);
                    if (artwork.isValid())
                        artworkSrc = shell.imageSource(artwork, artSize);
                }
                cards.append({ userId, displayName, avatarSrc, statusClass, typeIcon, typeText,
                    heading, details, state, artworkSrc, streaming });
            }
        }
    }

    if (cards.isEmpty()) {
        rml += QStringLiteral(
            "<div class=\"active-now-empty\">"
            "<div class=\"active-now-empty-heading\">It's quiet for now...</div>"
            "<div class=\"active-now-empty-text\">When friends are playing games, hanging out in voice, or listening to music, they'll show up here.</div>"
            "</div>");
    } else {
        for (const auto &card : cards) {
            QString detailsPart;
            if (!card.details.isEmpty()) {
                detailsPart = QStringLiteral("<div class=\"active-card-details\">%1</div>").arg(Rml::escape(card.details));
            }
            QString statePart;
            if (!card.state.isEmpty()) {
                statePart = QStringLiteral("<div class=\"active-card-state\">%1</div>").arg(Rml::escape(card.state));
            }
            const QString artwork = card.artworkSrc.isEmpty() ? QString() : QStringLiteral(
                "<div class=\"active-card-art\"><img src=\"%1\"/></div>").arg(escape(card.artworkSrc));

            rml += QStringLiteral("<div class=\"active-card%1\" data-user-id=\"%2\">")
                .arg(card.streaming ? QStringLiteral(" streaming") : QString(), card.userId.toString());
            rml += QStringLiteral(
                "<div class=\"active-card-top\">"
                "<div class=\"active-card-avatar-wrap\">"
                "<div class=\"active-card-avatar-clip\">"
                "<img class=\"active-card-avatar\" src=\"%1\"/>"
                "</div>"
                "<div class=\"status-dot %2\"></div>"
                "</div>"
                "<div class=\"active-card-user-info\">"
                "<span class=\"active-card-user\">%3</span>"
                "<span class=\"active-card-activity-type\">%4</span>"
                "</div>"
                "<img class=\"active-card-type-icon\" src=\"%5\"/>"
                "</div>"
                "<div class=\"active-card-body\">"
                "%9"
                "<div class=\"active-card-text\">"
                "<div class=\"active-card-heading\">%6</div>"
                "%7"
                "%8"
                "</div>"
                "</div>"
                "</div>")
                .arg(escape(card.avatarSrc), card.statusClass, escape(card.displayName),
                     escape(card.activityType), card.typeIcon, escape(card.heading),
                     detailsPart, statePart, artwork);
        }
    }

    rml += QStringLiteral("</div>"); // #active-now-list
}

void FriendsController::renderAddFriend(QString &rml)
{
    const bool canSend = !addFriendInputText.trimmed().isEmpty() && !addFriendPending;
    const QString btnClass = canSend ? QString() : QStringLiteral(" disabled");

    QString feedbackHtml;
    if (!addFriendFeedback.isEmpty()) {
        const QString feedbackClass = addFriendSuccess ? QStringLiteral("feedback-success") : QStringLiteral("feedback-error");
        feedbackHtml = QStringLiteral("<div class=\"add-friend-feedback %1\">%2</div>")
                           .arg(feedbackClass)
                           .arg(Rml::escape(addFriendFeedback));
    }

    rml += QStringLiteral(
        "<div class=\"add-friend-container\">"
        "<div class=\"add-friend-header\">ADD FRIEND</div>"
        "<div class=\"add-friend-subtitle\">You can add friends with their Discord Tag or username.</div>"
        "<div class=\"add-friend-input-wrap\">"
        "<input type=\"text\" id=\"add-friend-input\" class=\"add-friend-input\" value=\"%1\"%6/>"
        "<span id=\"add-friend-placeholder\" class=\"add-friend-placeholder\" style=\"%2\">You can add friends with their Discord Tag or username</span>"
        "<button id=\"add-friend-btn\" class=\"add-friend-btn%3\" data-action=\"send-friend-request\">%4</button>"
        "</div>"
        "%5"
        "</div>")
        .arg(Rml::escape(addFriendInputText),
             addFriendInputText.isEmpty() ? QString() : QStringLiteral("display: none;"),
             btnClass,
             addFriendPending ? QStringLiteral("Sending...") : QStringLiteral("Send Friend Request"),
             feedbackHtml,
             addFriendPending ? QStringLiteral(" disabled=\"disabled\"") : QString());
}

} // namespace Rml
} // namespace UI
} // namespace Styx
