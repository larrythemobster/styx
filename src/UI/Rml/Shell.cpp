#include "Shell.hpp"

#include "CallController.hpp"
#include "ChannelListController.hpp"
#include "ChatController.hpp"
#include "FriendsController.hpp"
#include "MemberListController.hpp"
#include "RailController.hpp"
#include "RmlRuntime.hpp"
#include "RmlView.hpp"
#include "UserPanelController.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/ForumManager.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Logging.hpp"
#include "Core/PendingAttachment.hpp"
#include "Core/PermissionManager.hpp"
#include "Core/MemberListManager.hpp"
#include "Core/ReadStateManager.hpp"
#include "Core/RelationshipManager.hpp"
#include "Core/Session.hpp"
#include "Core/TypingTracker.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"
#include "Discord/Entities.hpp"
#include "UI/Accounts/AccountsModel.hpp"
#include "UI/Accounts/AccountsWindow.hpp"
#include "UI/ChannelList/ChannelFilterProxyModel.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"
#include "UI/ChannelList/ServerRailModel.hpp"
#include "UI/Chat/ChatModel.hpp"
#include "UI/BrowserCaptchaResolver.hpp"
#include "UI/Dialogs/UserProfilePopup.hpp"
#include "UI/Forum/ForumBrowser.hpp"
#include "UI/Forum/ForumPostModel.hpp"
#include "UI/GuildSettings/GuildSettingsSection.hpp"
#include "UI/GuildSettings/GuildSettingsWindow.hpp"
#include "UI/GuildSettings/MemberModeration.hpp"
#include "UI/ImageViewer.hpp"
#include "UI/MemberList/MemberListModel.hpp"
#include "UI/Settings/SettingsWindow.hpp"
#include "UI/ThreadBrowser/ThreadBrowserPopup.hpp"
#include "UI/TrayIcon.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <RmlUi/Core/ElementDocument.h>

namespace Styx {
namespace UI {

using Core::Snowflake;

namespace {

QString guildChannelKey(Core::Snowflake accountId, Core::Snowflake guildId)
{
    return QStringLiteral("%1:%2")
            .arg(static_cast<quint64>(accountId))
            .arg(static_cast<quint64>(guildId));
}

QWidget *buildUserMenuHeader(QMenu *parent, Core::Session *session, Snowflake accountId,
                             const QString &displayName, const QString &username,
                             const QUrl &avatarUrl)
{
    auto *header = new QWidget(parent);
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(10, 10, 16, 10);
    layout->setSpacing(12);

    constexpr QSize avatarSize(64, 64);
    auto *avatar = new QLabel(header);
    avatar->setFixedSize(avatarSize);
    session->getImageManager()->assign(avatar, avatarUrl, avatarSize, accountId);

    layout->addWidget(avatar);

    auto *textLayout = new QVBoxLayout;
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);

    auto *displayLabel = new QLabel(displayName, header);
    QFont displayFont = displayLabel->font();
    displayFont.setBold(true);
    displayFont.setPointSize(displayFont.pointSize() + 2);
    displayLabel->setFont(displayFont);
    textLayout->addWidget(displayLabel);

    if (!username.isEmpty() && username != displayName) {
        auto *usernameLabel = new QLabel(username, header);
        usernameLabel->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
        textLayout->addWidget(usernameLabel);
    }
    textLayout->addStretch();
    layout->addLayout(textLayout, 1);

    return header;
}

QWidget *buildRoleChip(const Discord::Role &role, QWidget *parent)
{
    auto *label = new QLabel(role.name.get(), parent);
    label->setContentsMargins(24, 4, 24, 4);
    if (role.hasColor())
        label->setStyleSheet(QStringLiteral("color: %1;").arg(role.getColor().name()));
    return label;
}

} // namespace

Shell::Shell(Core::Session *session, QWidget *parent) : QMainWindow(parent), coreSession(session)
{
    Rml::Runtime::instance().setImageManager(session->getImageManager());

    auto *captchaResolver = new BrowserCaptchaResolver(this, this);
    session->setCaptchaResolver(captchaResolver);

    chatModel = new ChatModel(session->getImageManager(), this);
    channelTreeModel = new ChannelTreeModel(session, this);
    channelFilterProxy = new ChannelFilterProxyModel(session, this);
    channelFilterProxy->setSourceModel(channelTreeModel);
    channelFilterProxy->setDynamicSortFilter(true);
    channelFilterProxy->sort(0);
    accountsModel = new AccountsModel(session, this);
    serverRailModel = new ServerRailModel(session, channelTreeModel, this);
    typingTracker = new Core::TypingTracker(this);
    memberListModel = new MemberListModel(session->getImageManager(), this);

    chatModel->setAvatarUrlResolver([this](const Discord::User &author, Snowflake guildId) -> QUrl {
        if (!instance)
            return Discord::Cdn::userAvatar(author.id.get(), author.avatar.get(), 64);
        return instance->users()->getAvatarUrl(author, guildId, 64);
    });

    chatModel->setDisplayNameResolver([this](const Discord::User &author, Snowflake guildId) -> QString {
        if (!instance)
            return QString();
        return instance->users()->getAuthorDisplayName(author, guildId);
    });

    chatModel->setRoleColorResolver(
            [this](Snowflake userId, Snowflake guildId) { return roleColor(userId, guildId); });

    chatModel->setChannelNameResolver([this](Snowflake channelId) -> QString {
        if (!instance)
            return QString();
        auto channel = instance->getChannel(channelId);
        if (channel && channel->name.hasValue())
            return channel->name.get();
        return QString();
    });

    chatModel->setGuildInfoResolver([this](Snowflake guildId) -> QPair<QString, QUrl> {
        if (!instance)
            return {};
        auto guild = instance->getGuild(guildId);
        if (!guild || !guild->name.hasValue())
            return {};
        QUrl iconUrl;
        if (guild->icon.hasValue() && !guild->icon.get().isEmpty())
            iconUrl = Discord::Cdn::guildIcon(guildId, guild->icon.get(), 64);
        return { guild->name.get(), iconUrl };
    });

    connect(session, &Core::Session::ready, this, [this](const Discord::Ready &ready) {
        channelTreeModel->populateFromReady(ready);
        if (!current.accountId.isValid()) {
            restoreLastLocation(ready.user->id);
        } else {
            QSettings s;
            Snowflake savedAcc(s.value("ui/rail/lastAccountId", static_cast<qulonglong>(Snowflake::Invalid)).toULongLong());
            if (savedAcc == ready.user->id)
                restoreLastLocation(ready.user->id);
        }
        emit accountReady(ready.user->id);
    });

    connect(accountsModel, &AccountsModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight,
                   const QVector<int> &) {
                for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
                    QModelIndex idx = accountsModel->index(row, 0);
                    auto accPtr = idx.data(AccountsModel::AccountObjectRole).value<void *>();
                    if (!accPtr)
                        continue;
                    auto acc = *static_cast<Styx::Core::AccountInfo *>(accPtr);
                    if (acc.state == Styx::Core::ConnectionState::Connected) {
                        channelTreeModel->addAccount(acc);
                        Core::ClientInstance *inst = coreSession->client(acc.id);
                        if (inst)
                            setupPermanentConnections(inst);
                    } else if (acc.state == Styx::Core::ConnectionState::Disconnected) {
                        if (instance && instance->accountId() == acc.id) {
                            switchActiveInstance(nullptr);
                        }
                        channelTreeModel->removeAccount(acc.id);
                        connectedInstances.remove(acc.id);
                    }
                }
            });

    for (const auto &inst : session->getClients()) {
        if (inst)
            setupPermanentConnections(inst);
    }

    applyCloseToTray();
    restoreWindowState();

    rmlView = new Rml::View(QStringLiteral("main"), this);
    setCentralWidget(rmlView);
    connect(rmlView, &Rml::View::documentLoaded, this, &Shell::onDocumentLoaded);
    rmlView->loadDocument(QStringLiteral("main.rml"));

    setWindowTitle(QStringLiteral("Styx"));
    resize(1280, 800);
}

Shell::~Shell()
{
    for (const QPointer<GuildSettingsWindow> &window : std::as_const(guildSettingsWindows))
        delete window.data();
    if (forumWindow)
        delete forumWindow.data();
    if (threadBrowser)
        delete threadBrowser.data();
}

Core::ImageManager *Shell::images() const
{
    return coreSession->getImageManager();
}

Core::ClientInstance *Shell::instanceFor(Snowflake accountId) const
{
    return coreSession->client(accountId);
}

::Rml::ElementDocument *Shell::document() const
{
    return rmlView ? rmlView->document() : nullptr;
}

QString Shell::imageSource(const QUrl &url, const QSize &logicalSize) const
{
    return Rml::Runtime::instance().imageSource(url, logicalSize, current.accountId);
}

void Shell::onDocumentLoaded(::Rml::ElementDocument *doc)
{
    auto region = [doc](const char *id) { return doc->GetElementById(id); };
    controllers.push_back(std::make_unique<Rml::RailController>(*this, region("rail")));
    controllers.push_back(std::make_unique<Rml::ChannelListController>(*this, region("sidebar")));
    controllers.push_back(std::make_unique<Rml::UserPanelController>(*this, region("sidebar")));
    controllers.push_back(std::make_unique<Rml::ChatController>(*this, region("main")));
    controllers.push_back(std::make_unique<Rml::MemberListController>(*this, region("members")));
    controllers.push_back(std::make_unique<Rml::CallController>(*this, region("call")));
    controllers.push_back(std::make_unique<Rml::FriendsController>(*this, region("friends")));
    applyBodyClasses();
}

void Shell::applyBodyClasses()
{
    if (auto *doc = document()) {
        doc->SetClass("call-open", callOpen);
        doc->SetClass("members-hidden", !memberListVisible);
        doc->SetClass("friends-open", isFriendsOpen());
    }
    if (rmlView)
        rmlView->requestRender(200);
}

void Shell::setCallOpen(bool open)
{
    if (callOpen == open)
        return;
    callOpen = open;
    applyBodyClasses();
    emit callOpenChanged(open);
}

void Shell::setMemberListVisible(bool visible)
{
    if (memberListVisible == visible)
        return;
    memberListVisible = visible;
    applyBodyClasses();
    emit memberListVisibleChanged(visible);
}

void Shell::closeEvent(QCloseEvent *event)
{
    saveWindowState();

    const bool reachableFromTray = trayIcon && QSystemTrayIcon::isSystemTrayAvailable();
    qApp->setQuitOnLastWindowClosed(!reachableFromTray);
    hide();

    event->accept();
}

void Shell::showFromTray()
{
    setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
}

void Shell::quitFromTray()
{
    saveWindowState();
    qApp->quit();
}

void Shell::applyCloseToTray()
{
    const bool wanted = QSettings().value("general/close_to_tray", false).toBool();
    if (wanted == (trayIcon != nullptr))
        return;

    if (!wanted) {
        if (isHidden())
            showFromTray();
        delete trayIcon;
        trayIcon = nullptr;
        return;
    }

    trayIcon = new TrayIcon(this);
    trayIcon->setToolTip(windowTitle());
    connect(this, &QWidget::windowTitleChanged, trayIcon, &TrayIcon::setToolTip);
    connect(trayIcon, &TrayIcon::showWindowRequested, this, &Shell::showFromTray);
    connect(trayIcon, &TrayIcon::quitRequested, this, &Shell::quitFromTray);
}

void Shell::saveWindowState()
{
    QSettings settings;
    settings.setValue("layout/geometry", saveGeometry());

    if (serverRailModel)
        settings.setValue("ui/rail/expandedFolders", serverRailModel->expandedFolderKeys());

    settings.remove("ui/rail/lastChannels");
    settings.beginWriteArray("ui/rail/lastChannels");
    int lastChannelIdx = 0;
    for (auto it = lastViewedChannel.cbegin(); it != lastViewedChannel.cend(); ++it) {
        settings.setArrayIndex(lastChannelIdx++);
        settings.setValue("key", it.key());
        settings.setValue("channel", static_cast<qulonglong>(it.value()));
    }
    settings.endArray();
}

void Shell::restoreWindowState()
{
    QSettings settings;
    if (settings.contains("layout/geometry"))
        restoreGeometry(settings.value("layout/geometry").toByteArray());

    if (serverRailModel)
        serverRailModel->setExpandedFolderKeys(settings.value("ui/rail/expandedFolders").toStringList());

    int lastChannelCount = settings.beginReadArray("ui/rail/lastChannels");
    for (int i = 0; i < lastChannelCount; ++i) {
        settings.setArrayIndex(i);
        QString key = settings.value("key").toString();
        if (!key.isEmpty())
            lastViewedChannel.insert(key, Core::Snowflake(settings.value("channel").toULongLong()));
    }
    settings.endArray();
}

void Shell::restoreLastLocation(Core::Snowflake accountId)
{
    QSettings s;
    Snowflake savedAcc(s.value("ui/rail/lastAccountId", static_cast<qulonglong>(Snowflake::Invalid)).toULongLong());
    if (savedAcc.isValid() && savedAcc != accountId)
        return;

    bool savedIsHome = s.value("ui/rail/lastIsHome", true).toBool();
    Snowflake savedGuild(s.value("ui/rail/lastGuildId", static_cast<qulonglong>(Snowflake::Invalid)).toULongLong());

    if (!savedIsHome && savedGuild.isValid() && channelTreeModel->serverIndex(accountId, savedGuild).isValid())
        openGuild(accountId, savedGuild);
    else
        openHome(accountId);
}

bool Shell::channelReadable(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId) const
{
    if (!channelId.isValid())
        return false;
    ChannelNode *node = channelTreeModel->findChannelTreeNode(channelId, accountId);
    if (!node || (node->type != ChannelNode::Type::Channel && node->type != ChannelNode::Type::VoiceChannel))
        return false;
    ChannelNode *guild = ChannelTreeModel::findGuildNode(node);
    if (!guild || guild->id != guildId)
        return false;
    Core::ClientInstance *inst = instanceFor(accountId);
    if (inst && inst->permissions())
        return inst->permissions()->hasChannelPermission(accountId, channelId, Discord::Permission::VIEW_CHANNEL);
    return true;
}

Core::Snowflake Shell::firstReadableChannel(Core::Snowflake accountId, Core::Snowflake guildId) const
{
    QModelIndex guildSrc = channelTreeModel->serverIndex(accountId, guildId);
    if (!guildSrc.isValid())
        return {};
    ChannelNode *guildNode = channelTreeModel->nodeFromIndex(guildSrc);
    if (!guildNode)
        return {};

    Core::ClientInstance *inst = instanceFor(accountId);
    auto *perms = inst ? inst->permissions() : nullptr;
    auto readable = [&](Snowflake ch) {
        return !perms || perms->hasChannelPermission(accountId, ch, Discord::Permission::VIEW_CHANNEL);
    };

    for (const auto &child : guildNode->children) {
        if (child->type == ChannelNode::Type::Category)
            break;
        if (child->type == ChannelNode::Type::Channel && readable(child->id))
            return child->id;
    }

    for (const auto &child : guildNode->children) {
        if (child->type != ChannelNode::Type::Category)
            continue;
        for (const auto &cc : child->children) {
            if (cc->type == ChannelNode::Type::Channel && readable(cc->id))
                return cc->id;
        }
    }
    return {};
}

void Shell::setupPermanentConnections(Core::ClientInstance *inst)
{
    if (!inst)
        return;
    if (connectedInstances.contains(inst->accountId()))
        return;
    connectedInstances.insert(inst->accountId());

    connect(inst, &Core::ClientInstance::guildCreated, this,
            [this, inst](const Discord::GatewayGuild &guild) {
                channelTreeModel->addGuild(guild, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::guildUpdated, this,
            [this, inst](const Discord::Guild &guild) {
                channelTreeModel->updateGuild(guild, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::guildRemoved, this,
            [this, inst](Snowflake guildId) {
                channelTreeModel->removeGuild(inst->accountId(), guildId);
            });
    connect(inst->discord(), &Discord::Client::guildLeaveFailed, this,
            [this, inst](Snowflake guildId, const QString &error) {
                auto guild = inst->getGuild(guildId);
                QString guildName = guild.has_value() ? guild->name.get() : QString::number(guildId);
                QMessageBox::warning(this,
                                     tr("Failed to leave server"),
                                     tr("Could not leave <b>%1</b>.<br>Error: %2")
                                             .arg(guildName.toHtmlEscaped(), error));
            });
    connect(inst, &Core::ClientInstance::channelCreated, this,
            [this, inst](const Discord::ChannelCreate &event) {
                channelTreeModel->addChannel(event, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::channelUpdated, this,
            [this, inst](const Discord::ChannelUpdate &update) {
                channelTreeModel->updateChannel(update, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::channelDeleted, this,
            [this, inst](const Discord::ChannelDelete &event) {
                channelTreeModel->deleteChannel(event, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::threadCreated, this,
            [this, inst](const Discord::Channel &thread) {
                channelTreeModel->addThread(thread, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::threadUpdated, this,
            [this, inst](const Discord::Channel &thread) {
                channelTreeModel->updateThread(thread, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::threadDeleted, this,
            [this, inst](Core::Snowflake threadId, Core::Snowflake, Core::Snowflake) {
                channelTreeModel->removeThread(threadId, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::threadListSynced, this,
            [this, inst](Core::Snowflake guildId, const QList<Core::Snowflake> &parentIds,
                         const QList<Discord::Channel> &threads) {
                channelTreeModel->syncThreads(guildId, parentIds, threads, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::threadMembershipChanged, this,
            [this, inst](Core::Snowflake threadId) {
                Core::Snowflake acc = inst->accountId();
                if (inst->isThreadJoined(threadId)) {
                    if (channelTreeModel->findChannelTreeNode(threadId, acc))
                        channelTreeModel->promoteTemporaryThread(threadId);
                    else if (auto ch = inst->getChannel(threadId))
                        channelTreeModel->addThread(*ch, acc);
                    channelTreeModel->updateReadState(threadId, acc);
                } else if (chatModel->getActiveChannelId() == threadId) {
                    if (auto ch = inst->getChannel(threadId))
                        channelTreeModel->showTemporaryThread(*ch, acc);
                } else {
                    channelTreeModel->removeThread(threadId, acc);
                }
            });
    connect(inst, &Core::ClientInstance::guildRoleCreated, this,
            [this](const Discord::GuildRoleCreate &event) {
                if (event.guildId.hasValue()) {
                    channelTreeModel->invalidateGuildData(event.guildId.get());
                    roleColorCache.clear();
                    chatModel->refreshUsersInView({});
                }
            });
    connect(inst, &Core::ClientInstance::guildRoleUpdated, this,
            [this](const Discord::GuildRoleUpdate &event) {
                if (event.guildId.hasValue()) {
                    channelTreeModel->invalidateGuildData(event.guildId.get());
                    roleColorCache.clear();
                    chatModel->refreshUsersInView({});
                }
            });
    connect(inst, &Core::ClientInstance::guildRoleDeleted, this,
            [this](const Discord::GuildRoleDelete &event) {
                if (event.guildId.hasValue()) {
                    channelTreeModel->invalidateGuildData(event.guildId.get());
                    roleColorCache.clear();
                    chatModel->refreshUsersInView({});
                }
            });
    connect(inst, &Core::ClientInstance::readStateChanged, this,
            [this, inst](Core::Snowflake channelId) {
                channelTreeModel->updateReadState(channelId, inst->accountId());
            });
    connect(inst->presences(), &Core::PresenceManager::presencesChanged, this,
            [this, inst](const QList<Core::Snowflake> &userIds) {
                channelTreeModel->updatePresence(inst->accountId(), userIds);
            });
    connect(inst, &Core::ClientInstance::forumBadgeChanged, this,
            [this, inst](Core::Snowflake forumId) {
                channelTreeModel->updateForumBadge(forumId, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::forumJoinedPostsChanged, this,
            [this, inst](Core::Snowflake forumId) {
                channelTreeModel->updateForumThreads(forumId, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::channelLastMessageUpdated, this,
            [this, inst](Core::Snowflake channelId, Core::Snowflake messageId) {
                channelTreeModel->updateChannelLastMessageId(channelId, messageId, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::guildSettingsChanged, this,
            [this, inst](Core::Snowflake guildId) {
                channelTreeModel->updateGuildSettings(guildId, inst->accountId());
            });
    connect(inst, &Core::ClientInstance::voiceStateChanged, this,
            [this, inst](Core::Snowflake channelId, Core::Snowflake) {
                channelTreeModel->setAccountVoiceChannel(inst->accountId(), channelId);
            });
#ifndef STYX_NO_VOICE
    if (inst->voice()) {
        connect(inst->voice(), &Core::Audio::VoiceManager::channelVoiceMemberChanged, this,
                [this, inst](Core::Snowflake channelId, Core::Snowflake userId, bool joined) {
                    int count = inst->voice()->channelVoiceUserCount(channelId);
                    channelTreeModel->updateVoiceCount(channelId, count, inst->accountId());
                    channelTreeModel->updateVoiceParticipant(channelId, userId, joined, inst->accountId());
                });
        connect(inst->voice(), &Core::Audio::VoiceManager::participantVoiceStateChanged, this,
                [this, inst](Core::Snowflake channelId, Core::Snowflake userId) {
                    channelTreeModel->updateVoiceParticipantState(channelId, userId, inst->accountId());
                    if (userId == inst->accountId()) {
                        const auto *p = inst->voice()->participant(userId);
                        if (p) {
                            if (p->selfMute != selfMute) {
                                selfMute = p->selfMute;
                                emit selfMuteChanged(selfMute);
                            }
                            if (p->selfDeaf != selfDeaf) {
                                selfDeaf = p->selfDeaf;
                                emit selfDeafChanged(selfDeaf);
                            }
                        }
                    }
                });
    }
#endif
}

void Shell::switchActiveInstance(Core::ClientInstance *newInstance)
{
    if (instance == newInstance)
        return;

    if (instance) {
        auto *msgs = instance->messages();
        disconnect(msgs, nullptr, chatModel, nullptr);
        disconnect(msgs, nullptr, this, nullptr);
        disconnect(instance->discord(), &Discord::Client::typingStart, this, nullptr);
        disconnect(instance->discord(), &Discord::Client::messageCreated, this, nullptr);
        disconnect(instance->permissions(), nullptr, this, nullptr);
        disconnect(instance, &Core::ClientInstance::membersUpdated, this, nullptr);
        disconnect(instance->relationships(), nullptr, this, nullptr);

        memberListModel->setManager(nullptr);
        memberListModel->setPresenceManager(nullptr);
        typingTracker->clear();
        typingTracker->setUserManager(nullptr);
    }

    instance = newInstance;

    if (!instance) {
        emit instanceChanged(nullptr);
        return;
    }

    memberListModel->setAccount(instance->accountId());
    memberListModel->setPresenceManager(instance->presences());
    memberListModel->setManager(instance->memberList());

    chatModel->setAccount(instance->accountId());

    typingTracker->clear();
    typingTracker->setUserManager(instance->users());
    typingTracker->setCurrentUserId(instance->accountId());

    auto *msgs = instance->messages();
    connect(msgs, &Core::MessageManager::messagesReceived, chatModel, &ChatModel::handleIncomingMessages);
    connect(msgs, &Core::MessageManager::messageErrored, chatModel, &ChatModel::handleMessageErrored);
    connect(msgs, &Core::MessageManager::messageDeleted, chatModel, &ChatModel::handleMessageDeleted);
    connect(msgs, &Core::MessageManager::attachmentUploadProgress, chatModel, &ChatModel::handleUploadProgress);

    connect(instance->discord(), &Discord::Client::typingStart, this,
            [this](const Discord::TypingStart &event) {
                if (!instance)
                    return;
                if (event.member.hasValue() && event.guildId.hasValue())
                    instance->users()->saveMemberWithUser(event.guildId.get(), event.member.get());
                typingTracker->addTyper(event.channelId, event.userId,
                                        event.guildId.hasValue() ? std::optional<Snowflake>(event.guildId.get())
                                                                 : std::nullopt);
            });
    connect(instance->discord(), &Discord::Client::messageCreated, this,
            [this](const Discord::Message &msg) {
                if (msg.author.hasValue())
                    typingTracker->removeTyper(msg.channelId, msg.author->id);
            });

    connect(instance, &Core::ClientInstance::membersUpdated, this,
            [this](Snowflake, const QList<Snowflake> &userIds) {
                for (const auto &userId : userIds)
                    roleColorCache.remove(userId);
                chatModel->refreshUsersInView(userIds);
            });

    connect(instance->relationships(), &Core::RelationshipManager::relationshipChanged, this,
            [this](Snowflake userId) {
                chatModel->refreshUsersInView({ userId });
            });

    emit instanceChanged(instance);
}

QColor Shell::roleColor(Core::Snowflake userId, Core::Snowflake guildId)
{
    if (!instance || !guildId.isValid())
        return QColor();

    if (roleColorGuild != guildId) {
        roleColorGuild = guildId;
        roleColorCache.clear();
    } else if (auto it = roleColorCache.constFind(userId); it != roleColorCache.constEnd()) {
        return it.value();
    }

    QColor result;
    for (const auto &role : instance->getMemberRolesSorted(guildId, userId)) {
        if (role.hasColor()) {
            result = role.getColor();
            break;
        }
    }

    roleColorCache[userId] = result;
    return result;
}

void Shell::openFriends(Core::Snowflake accountId)
{
    ClientInstance *inst = instanceFor(accountId);
    if (!inst)
        return;
    if (inst != instance)
        switchActiveInstance(inst);

    serverRailModel->setSelected(ServerRailModel::Kind::AccountHome, accountId, accountId);

    current = { accountId, Snowflake::Invalid, Snowflake::Invalid };
    channelFilterProxy->setSelectedChannel(Snowflake::Invalid, accountId);
    chatModel->setActiveChannel(Snowflake::Invalid, Snowflake::Invalid);
    if (inst->memberList())
        inst->memberList()->clear();
    applyBodyClasses();
    emit locationChanged(current);

    QSettings s;
    s.setValue("ui/rail/lastAccountId", static_cast<qulonglong>(accountId));
    s.setValue("ui/rail/lastIsHome", true);
}

bool Shell::isFriendsOpen() const
{
    return current.isHome() && !current.channelId.isValid();
}

void Shell::openHome(Core::Snowflake accountId)
{
    openFriends(accountId);
}

void Shell::openGuild(Core::Snowflake accountId, Core::Snowflake guildId)
{
    ClientInstance *inst = instanceFor(accountId);
    if (!inst)
        return;
    if (inst != instance)
        switchActiveInstance(inst);

    serverRailModel->setSelected(ServerRailModel::Kind::Server, accountId, guildId);

    Snowflake targetChannelId;
    QString key = guildChannelKey(accountId, guildId);
    auto it = lastViewedChannel.constFind(key);
    if (it != lastViewedChannel.cend() && channelReadable(accountId, guildId, it.value())) {
        targetChannelId = it.value();
    } else {
        QModelIndex guildSrc = channelTreeModel->serverIndex(accountId, guildId);
        ChannelNode *guildNode = guildSrc.isValid() ? channelTreeModel->nodeFromIndex(guildSrc) : nullptr;
        if (guildNode && channelReadable(accountId, guildId, guildNode->rulesChannelId))
            targetChannelId = guildNode->rulesChannelId;
        else
            targetChannelId = firstReadableChannel(accountId, guildId);
    }

    if (targetChannelId.isValid()) {
        openChannel(accountId, guildId, targetChannelId);
    } else {
        current = { accountId, guildId, Snowflake::Invalid };
        channelFilterProxy->setSelectedChannel(Snowflake::Invalid, accountId);
        chatModel->setActiveChannel(Snowflake::Invalid, guildId);
        applyBodyClasses();
        emit locationChanged(current);
    }

    QSettings s;
    s.setValue("ui/rail/lastAccountId", static_cast<qulonglong>(accountId));
    s.setValue("ui/rail/lastGuildId", static_cast<qulonglong>(guildId));
    s.setValue("ui/rail/lastIsHome", false);
}

void Shell::openChannel(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId)
{
    ClientInstance *inst = instanceFor(accountId);
    if (!inst)
        return;
    if (inst != instance)
        switchActiveInstance(inst);

    ChannelNode *node = channelTreeModel->findChannelTreeNode(channelId, accountId);
    if (!node) {
        auto chOpt = inst->getChannel(channelId);
        if (chOpt && chOpt->isThread()) {
            channelTreeModel->showTemporaryThread(*chOpt, accountId);
            node = channelTreeModel->findChannelTreeNode(channelId, accountId);
        }
    }

    if (!guildId.isValid() && node) {
        ChannelNode *gNode = ChannelTreeModel::findGuildNode(node);
        if (gNode)
            guildId = gNode->id;
    }

    ChannelNode *forumNode = (node && node->type == ChannelNode::Type::Thread) ? node->parent : node;
    if (forumNode && forumNode->type == ChannelNode::Type::Forum) {
        openForum(accountId, guildId, forumNode->id);
        if (node && node->type == ChannelNode::Type::Thread) {
            // thread inside forum also opens in chat below
        } else {
            return;
        }
    }

    bool isVoice = (node && node->type == ChannelNode::Type::VoiceChannel);
    if (isVoice) {
        joinVoice(accountId, guildId, channelId);
        setCallOpen(true);
    } else {
        if (callOpen)
            setCallOpen(false);
    }

    channelFilterProxy->setSelectedChannel(channelId, accountId);
    channelTreeModel->clearTemporaryThread(channelId);

    inst->readState()->setActiveChannel(channelId);

    bool isDm = !guildId.isValid();
    if (node)
        isDm = (node->type == ChannelNode::Type::DMChannel);

    if (isDm) {
        if (inst->memberList())
            inst->memberList()->clear();
    } else if (guildId.isValid()) {
        Snowflake threadParentId = Snowflake::Invalid;
        if (auto ch = inst->getChannel(channelId); ch && ch->isThread())
            threadParentId = ch->parentId.hasValue() ? ch->parentId.get() : Snowflake::Invalid;
        inst->memberList()->setActiveChannel(guildId, threadParentId.isValid() ? threadParentId : channelId);
    }

    if (guildId != roleColorGuild) {
        roleColorGuild = guildId;
        roleColorCache.clear();
    }

    chatModel->setActiveChannel(channelId, guildId);
    typingTracker->setActiveChannel(channelId);

    inst->messages()->requestLoadChannel(channelId);

    lastViewedChannel.insert(guildChannelKey(accountId, guildId), channelId);

    current = { accountId, guildId, channelId };
    applyBodyClasses();
    emit locationChanged(current);
}

void Shell::openDmWithUser(Core::Snowflake accountId, Core::Snowflake userId)
{
    ClientInstance *inst = instanceFor(accountId);
    if (!inst)
        return;
    inst->openDmChannel(userId, [this, accountId](const Core::Result<Snowflake> &res) {
        if (res.success())
            openChannel(accountId, Snowflake::Invalid, *res.value);
    });
}

void Shell::jumpToMessage(Core::Snowflake channelId, Core::Snowflake messageId)
{
    if (!instance)
        return;
    Snowflake accId = instance->accountId();

    if (chatModel->getActiveChannelId() == channelId) {
        instance->messages()->requestLoadAround(channelId, messageId);
        return;
    }

    ChannelNode *node = channelTreeModel->findChannelTreeNode(channelId, accId);
    if (!node) {
        auto chOpt = instance->getChannel(channelId);
        if (chOpt && chOpt->isThread())
            channelTreeModel->showTemporaryThread(*chOpt, accId);
        node = channelTreeModel->findChannelTreeNode(channelId, accId);
    }

    Snowflake guildId = Snowflake::Invalid;
    if (node) {
        ChannelNode *gNode = ChannelTreeModel::findGuildNode(node);
        if (gNode)
            guildId = gNode->id;
    }

    openChannel(accId, guildId, channelId);
    instance->messages()->requestLoadAround(channelId, messageId);
}

void Shell::joinVoice(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId)
{
    ClientInstance *inst = instanceFor(accountId);
    if (!inst)
        return;
    if (inst != instance)
        switchActiveInstance(inst);

#ifndef STYX_NO_VOICE
    if (!inst->discord()->getProxy().canRelayUdp()) {
        QMessageBox::warning(this,
                             tr("Voice unavailable"),
                             tr("This account uses an HTTP proxy, which cannot carry voice traffic.<br>Switch it to a SOCKS5 proxy to use voice."));
        return;
    }

    qCInfo(LogVoice) << "Joining voice channel" << channelId << "in guild" << guildId;
    inst->discord()->sendVoiceStateUpdate(guildId, channelId, selfMute, selfDeaf);
#else
    Q_UNUSED(guildId);
    Q_UNUSED(channelId);
#endif
}

void Shell::leaveVoice()
{
    if (!instance)
        return;
#ifndef STYX_NO_VOICE
    if (instance->isInVoice()) {
        qCInfo(LogVoice) << "Disconnecting from voice";
        instance->discord()->sendVoiceStateUpdate(instance->voiceGuildId(), Snowflake::Invalid, false, false);
    }
#endif
    setCallOpen(false);
}

void Shell::setSelfMute(bool muted)
{
    selfMute = muted;
    if (instance && instance->isInVoice()) {
        instance->discord()->sendVoiceStateUpdate(instance->voiceGuildId(), instance->voiceChannelId(), selfMute, selfDeaf);
    }
    emit selfMuteChanged(muted);
}

void Shell::setSelfDeaf(bool deafened)
{
    selfDeaf = deafened;
    if (instance && instance->isInVoice()) {
        instance->discord()->sendVoiceStateUpdate(instance->voiceGuildId(), instance->voiceChannelId(), selfMute, selfDeaf);
    }
    emit selfDeafChanged(deafened);
}

bool Shell::sendMessage(const QString &text, Core::Snowflake replyTo)
{
    return sendMessage(text, {}, replyTo);
}

bool Shell::sendMessage(const QString &text, const QList<Core::PendingAttachment> &attachments, Core::Snowflake replyTo)
{
    if (!instance) {
        qCWarning(LogCore) << "Cannot send message: no active instance";
        return false;
    }
    Snowflake channelId = chatModel->getActiveChannelId();
    if (!channelId.isValid()) {
        qCWarning(LogCore) << "Cannot send message: no active channel";
        return false;
    }

    instance->messages()->sendMessage(channelId, text, replyTo, attachments);
    return true;
}

void Shell::markChannelRead(Core::Snowflake channelId)
{
    if (!channelId.isValid())
        return;
    Snowflake accId = current.accountId;
    if (!accId.isValid() && instance)
        accId = instance->accountId();
    ClientInstance *inst = instanceFor(accId);
    if (!inst)
        return;

    ChannelNode *node = channelTreeModel->findChannelTreeNode(channelId, accId);
    if (node) {
        QModelIndex srcIdx = channelTreeModel->indexForNode(node);
        if (srcIdx.isValid()) {
            inst->readState()->markChannelsAsRead(channelTreeModel->getMarkableChannelIds(srcIdx));
            return;
        }
    }

    Snowflake lastMsgId = inst->readState()->getChannelLastMessageId(channelId);
    inst->readState()->markChannelAsRead(channelId, lastMsgId);
}

void Shell::markGuildRead(Core::Snowflake accountId, Core::Snowflake guildId)
{
    ClientInstance *inst = instanceFor(accountId);
    if (!inst)
        return;
    inst->markGuildsAsRead({ guildId });
}

void Shell::showUserProfile(Core::Snowflake userId, Core::Snowflake guildId)
{
    if (!instance)
        return;
    auto *popup = new UserProfilePopup(images(), instance, userId, guildId, this);
    connect(popup, &UserProfilePopup::linkActivated, this, [](const QString &url) {
        QDesktopServices::openUrl(QUrl(url));
    });
    popup->show();
}

void Shell::openSettings()
{
    if (!settingsWindow) {
        settingsWindow = new SettingsWindow(this);
        connect(settingsWindow, &SettingsWindow::closeToTrayChanged, this, &Shell::applyCloseToTray);
    }
    settingsWindow->show();
    settingsWindow->raise();
    settingsWindow->activateWindow();
}

void Shell::openAccounts()
{
    if (!accountsWindow)
        accountsWindow = new AccountsWindow(coreSession, accountsModel, this);
    accountsWindow->show();
    accountsWindow->raise();
    accountsWindow->activateWindow();
}

void Shell::openGuildSettings(Core::Snowflake accountId, Core::Snowflake guildId)
{
    ClientInstance *inst = instanceFor(accountId);
    if (!inst || !GuildSettingsAccess::canOpen(inst, guildId))
        return;

    QPointer<GuildSettingsWindow> &window = guildSettingsWindows[qMakePair(accountId, guildId)];
    if (!window) {
        window = new GuildSettingsWindow(images(), inst, guildId);
        connect(window, &GuildSettingsWindow::linkActivated, this, [](const QString &url) {
            QDesktopServices::openUrl(QUrl(url));
        });
    }
    window->openSection(GuildSettingsSection::Profile);
    window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    window->show();
    window->raise();
    window->activateWindow();
}

void Shell::openImage(const QUrl &url)
{
    auto *viewer = new ImageViewer(images(), current.accountId, this);
    viewer->showImage(url, QPixmap());
}

void Shell::openForum(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake forumId)
{
    ClientInstance *inst = instanceFor(accountId);
    if (!inst)
        return;

    if (!forumModel) {
        forumModel = new ForumPostModel(images(), this);
        forumModel->setRoleColorResolver([this](Snowflake userId, Snowflake gId) { return roleColor(userId, gId); });
    }
    forumModel->setAccount(accountId);
    forumModel->setManager(inst->forums());
    forumModel->setForum(forumId, guildId);

    if (!forumWindow) {
        forumWindow = new QWidget(nullptr, Qt::Window);
        forumWindow->setWindowTitle(tr("Forum"));
        forumWindow->resize(800, 600);
        auto *layout = new QVBoxLayout(forumWindow);
        layout->setContentsMargins(0, 0, 0, 0);
        forumBrowser = new ForumBrowser(forumWindow);
        forumBrowser->setModel(forumModel);
        layout->addWidget(forumBrowser);

        connect(forumBrowser, &ForumBrowser::postActivated, this, [this, accountId](Snowflake threadId, Snowflake gId) {
            openChannel(accountId, gId, threadId);
        });
        connect(forumBrowser, &ForumBrowser::sortModeChanged, this, [this, inst, forumId](int mode) {
            inst->forums()->setSortMode(forumId, static_cast<Core::ForumSortMode>(mode));
        });
    }

    QString name;
    if (auto ch = inst->getChannel(forumId); ch && ch->name.hasValue())
        name = ch->name.get();
    forumWindow->setWindowTitle(name.isEmpty() ? tr("Forum") : tr("#%1").arg(name));

    Snowflake newestPost = inst->readState()->getChannelLastMessageId(forumId);
    if (newestPost.isValid())
        inst->readState()->markChannelAsRead(forumId, newestPost);
    inst->readState()->setActiveChannel(forumId);

    forumBrowser->setSortMode(static_cast<int>(inst->forums()->sortMode(forumId)));
    forumBrowser->setLoading(inst->forums()->isLoading(forumId));
    forumWindow->show();
    forumWindow->raise();
    forumWindow->activateWindow();
}

void Shell::openThreadBrowser(Core::Snowflake channelId)
{
    if (!instance || !channelId.isValid())
        return;

    if (!threadBrowser) {
        threadBrowser = new ThreadBrowserPopup(this);
        connect(threadBrowser, &ThreadBrowserPopup::threadActivated, this, [this](Core::Snowflake threadId) {
            if (instance)
                openChannel(instance->accountId(), current.guildId, threadId);
        });
    }

    QString name;
    if (auto ch = instance->getChannel(channelId); ch && ch->name.hasValue())
        name = ch->name.get();

    threadBrowser->configure(instance, channelId, name);
    threadBrowser->show();
    threadBrowser->raise();
    threadBrowser->activateWindow();
}

void Shell::showContextMenuForUser(Core::Snowflake userId, Core::Snowflake guildId)
{
    QMenu menu(this);
    ClientInstance *inst = instance;
    QPointer<ClientInstance> instanceGuard(inst);

    if (inst) {
        auto user = inst->users()->getUser(userId);
        QString displayName = inst->users()->getDisplayName(userId, guildId);
        QString username = (user && user->username.hasValue()) ? user->username.get() : QString();
        QUrl avatarUrl = user ? inst->users()->getAvatarUrl(*user, guildId, 128)
                              : Discord::Cdn::defaultUserAvatar(userId, 128);

        auto *header = buildUserMenuHeader(&menu, coreSession, inst->accountId(),
                                           displayName, username, avatarUrl);
        auto *headerAction = new QWidgetAction(&menu);
        headerAction->setDefaultWidget(header);
        menu.addAction(headerAction);
        menu.addSeparator();
    }

    QAction *profileAction = menu.addAction(tr("Profile"));
    connect(profileAction, &QAction::triggered, this, [this, userId, guildId]() {
        showUserProfile(userId, guildId);
    });

    QAction *mentionAction = menu.addAction(tr("Mention"));
    mentionAction->setEnabled(inst != nullptr);
    connect(mentionAction, &QAction::triggered, this, [userId]() {
        QGuiApplication::clipboard()->setText(QStringLiteral("<@%1>").arg(quint64(userId)));
    });

    QAction *openDmAction = menu.addAction(tr("Open DM"));
    openDmAction->setEnabled(inst && userId != inst->accountId());
    connect(openDmAction, &QAction::triggered, this, [this, instanceGuard, userId]() {
        if (!instanceGuard)
            return;
        openDmWithUser(instanceGuard->accountId(), userId);
    });

    if (guildId.isValid() && inst) {
        const auto target = MemberModeration::targetFor(inst, guildId, userId);
        const bool moderationAddsRolesMenu = target && MemberModeration::canEditRoles(inst, *target);
        if (!moderationAddsRolesMenu) {
            QMenu *rolesMenu = menu.addMenu(tr("Roles"));
            const auto memberRoles = inst->getMemberRolesSorted(guildId, userId);
            if (memberRoles.isEmpty()) {
                rolesMenu->addAction(tr("No roles"))->setEnabled(false);
            } else {
                for (const auto &role : memberRoles) {
                    auto *action = new QWidgetAction(rolesMenu);
                    action->setDefaultWidget(buildRoleChip(role, rolesMenu));
                    rolesMenu->addAction(action);
                }
            }
        }
        if (target)
            MemberModeration::addModerationActions(&menu, this, inst, *target);
    }

    menu.addSeparator();
    QAction *copyIdAction = menu.addAction(tr("Copy User ID"));
    connect(copyIdAction, &QAction::triggered, this, [userId]() {
        QGuiApplication::clipboard()->setText(QString::number(quint64(userId)));
    });

    menu.exec(QCursor::pos());
}

void Shell::showContextMenuForGuild(Core::Snowflake accountId, Core::Snowflake guildId)
{
    ClientInstance *inst = instanceFor(accountId);
    if (!inst)
        return;

    QMenu menu(this);

    QAction *markRead = menu.addAction(tr("Mark As Read"));
    connect(markRead, &QAction::triggered, this, [this, accountId, guildId]() {
        markGuildRead(accountId, guildId);
    });

    GuildSettingsAccess::addMenu(&menu, [this](Core::Snowflake accId, Core::Snowflake gId) {
        return GuildSettingsAccess::visibleSections(instanceFor(accId), gId);
    }, accountId, guildId, [this, accountId, guildId](GuildSettingsSection) {
        openGuildSettings(accountId, guildId);
    });

    menu.addSeparator();

    auto guild = inst->getGuild(guildId);
    bool isOwner = guild && guild->ownerId.hasValue() && guild->ownerId.get() == accountId;

    QAction *leaveAction = menu.addAction(tr("Leave Server"));
    leaveAction->setEnabled(!isOwner);
    connect(leaveAction, &QAction::triggered, this, [inst, guildId]() {
        inst->discord()->leaveGuild(guildId);
    });

    menu.addSeparator();
    QAction *copyIdAction = menu.addAction(tr("Copy Server ID"));
    connect(copyIdAction, &QAction::triggered, this, [guildId]() {
        QGuiApplication::clipboard()->setText(QString::number(quint64(guildId)));
    });

    menu.exec(QCursor::pos());
}

void Shell::showContextMenuForChannel(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId)
{
    ClientInstance *inst = instanceFor(accountId);
    if (!inst)
        return;

    QMenu menu(this);

    ChannelNode *node = channelTreeModel->findChannelTreeNode(channelId, accountId);
    bool isVoice = (node && node->type == ChannelNode::Type::VoiceChannel);
    bool isDm = (node && node->type == ChannelNode::Type::DMChannel);

    if (isVoice || isDm) {
        QString joinText = isDm ? tr("Join Call") : tr("Join Voice Channel");
        QAction *joinAction = menu.addAction(joinText);
#ifdef STYX_NO_VOICE
        joinAction->setEnabled(false);
#else
        connect(joinAction, &QAction::triggered, this, [this, accountId, guildId, channelId]() {
            joinVoice(accountId, guildId, channelId);
            setCallOpen(true);
        });
#endif

        bool inVoice = inst->isInVoice() && inst->voiceChannelId() == channelId;
        QAction *disconnectAction = menu.addAction(tr("Disconnect from Voice"));
#ifdef STYX_NO_VOICE
        disconnectAction->setEnabled(false);
#else
        disconnectAction->setEnabled(inVoice);
        connect(disconnectAction, &QAction::triggered, this, [this]() {
            leaveVoice();
        });
#endif
        menu.addSeparator();
    }

    QAction *markReadAction = menu.addAction(tr("Mark As Read"));
    connect(markReadAction, &QAction::triggered, this, [this, channelId]() {
        markChannelRead(channelId);
    });

    menu.addSeparator();
    QAction *copyIdAction = menu.addAction(tr("Copy Channel ID"));
    connect(copyIdAction, &QAction::triggered, this, [channelId]() {
        QGuiApplication::clipboard()->setText(QString::number(quint64(channelId)));
    });

    menu.exec(QCursor::pos());
}

} // namespace UI
} // namespace Styx
