#include "Shell.hpp"
#ifndef STYX_NO_VOICE
#include "CallController.hpp"
#endif
#include "ChannelListController.hpp"
#include "ChatController.hpp"
#include "FriendsController.hpp"
#include "MemberListController.hpp"
#include "RailController.hpp"
#include "RmlRuntime.hpp"
#include "UserPanelController.hpp"
#include "AccountsController.hpp"
#include "ConfirmModal.hpp"
#include "UserProfileController.hpp"
#include "ForumController.hpp"
#include "GuildSettingsSection.hpp"
#include "GuildSettingsController.hpp"
#include "MemberModeration.hpp"
#include "ImageViewerController.hpp"
#include "SettingsController.hpp"
#include "ThreadBrowserController.hpp"
#include "SearchController.hpp"
#include "ExpressionPickerController.hpp"
#ifndef STYX_NO_VOICE
#include "Core/Audio/VoiceManager.hpp"
#endif
#include "Core/Audio/Sounds.hpp"
#include "Core/ForumManager.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Logging.hpp"
#include "Core/NotificationText.hpp"
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
#include "UI/ChannelList/ChannelFilterProxyModel.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"
#include "UI/ChannelList/ServerRailModel.hpp"
#include "UI/Chat/ChatModel.hpp"
#include "UI/BrowserCaptchaResolver.hpp"
#include "UI/MemberList/MemberListModel.hpp"
#include "UI/TrayIcon.hpp"
#include "UI/CallSounds.hpp"
#include "Platform/Desktop.hpp"
#include "Platform/Settings.hpp"
#include "Platform/Notifications.hpp"
#include "Platform/Badge.hpp"
#include "Core/SettingsKeys.hpp"
#include "App.hpp"
#ifdef STYX_HAVE_SCREENSHARE
#include "GoLiveController.hpp"
#endif
#include <RmlUi/Core/ElementDocument.h>
#include <algorithm>
#include <format>
#include <filesystem>
#include <system_error>

namespace Styx::UI {
using Core::Snowflake;
using Core::ClientInstance;
namespace {
Shell *primary = nullptr;
std::string guildChannelKey(Snowflake account, Snowflake guild) { return account.toString() + ":" + guild.toString(); }

void showMessageNotification(Shell *shell, Core::ClientInstance *client, const Discord::Message &msg)
{
    const Snowflake account = client->accountId();
    const Snowflake channelId = msg.channelId.get();
    const Snowflake guildId = msg.guildId.hasValue() ? msg.guildId.get() : Snowflake::Invalid;
    const Snowflake messageId = msg.id.get();
    auto *users = client->users();
    const std::string author = msg.author.hasValue() ? users->getAuthorDisplayName(msg.author.get(), guildId) : std::string();
    auto channel = client->getChannel(channelId);

    std::string title;
    if (channel && channel->type.hasValue() && channel->type.get() == Discord::ChannelType::GROUP_DM) {
        std::string groupName;
        if (channel->name.hasValue() && !channel->name.get().empty()) {
            groupName = channel->name.get();
        } else if (channel->recipients.hasValue()) {
            for (const auto &user : channel->recipients.get()) {
                const std::string name = users->getDisplayName(user.id.get());
                if (name.empty())
                    continue;
                if (!groupName.empty())
                    groupName += ", ";
                groupName += name;
            }
        }
        title = groupName.empty() ? author : author + " (" + groupName + ")";
    } else if ((channel && channel->type.hasValue() && channel->type.get() == Discord::ChannelType::DM) || (!guildId.isValid() && !channel)) {
        title = author;
    } else {
        const std::string channelName = channel ? channel->name.valueOr(std::string()) : std::string();
        auto guild = guildId.isValid() ? client->getGuild(guildId) : std::nullopt;
        const std::string guildName = (guild && guild->name.hasValue()) ? guild->name.get() : std::string();
        if (!channelName.empty() && !guildName.empty())
            title = author + " (#" + channelName + ", " + guildName + ")";
        else if (!channelName.empty())
            title = author + " (#" + channelName + ")";
        else
            title = author;
    }

    const int attachmentCount = msg.attachments.hasValue() ? static_cast<int>(msg.attachments.get().size()) : 0;
    const int stickerCount = msg.stickerItems.hasValue() ? static_cast<int>(msg.stickerItems.get().size()) : 0;

    Platform::Notifications::Notification notification;
    notification.title = std::move(title);
    notification.body = Core::notificationText(msg.content.valueOr(std::string()), attachmentCount, stickerCount, client->mentionNames(guildId));

    if (msg.author.hasValue()) {
        const std::string avatar = users->getAvatarUrl(msg.author.get(), guildId, 64);
        // Only an avatar that's already on disk: a notification can't wait for a download.
        if (auto *images = shell->images(); images && images->isCached(avatar, {64, 64})) {
            const std::string path = images->rawDownloadPath(avatar, {64, 64});
            std::error_code ec;
            if (!path.empty() && std::filesystem::exists(path, ec) && !ec)
                notification.image = path;
        }
    }

    Platform::Guard<Shell> guard(shell);
    Platform::Notifications::show(notification, [guard, account, guildId, channelId, messageId] {
        if (!guard)
            return;
        if (!guard->isMaximized())
            guard->showNormal();
        else
            guard->show();
        guard->raise();
        guard->activateWindow();
        guard->openChannel(account, guildId, channelId);
        guard->jumpToMessage(channelId, messageId);
    });
}
}
Shell *Shell::primaryShell() { return primary; }
Shell::Shell(Core::Session *session) : View("main"), coreSession(session)
{
    primary = this;
    Rml::Runtime::instance().setImageManager(session->getImageManager());
    captchaResolver = std::make_unique<BrowserCaptchaResolver>(*this);
    session->setCaptchaResolver(captchaResolver.get());
    chatModel = std::make_unique<ChatModel>(session->getImageManager());
    channelTreeModel = std::make_unique<ChannelTreeModel>(session);
    channelFilterProxy = std::make_unique<ChannelFilterProxyModel>(session);
    channelFilterProxy->setSourceModel(channelTreeModel.get());
    track(channelTreeModel->totalMentionCountChanged.connect([this](int total, int) { mentionTotal = total; refreshTaskbarBadge(); }));
    accountsModel = std::make_unique<AccountsModel>(session);
    serverRailModel = std::make_unique<ServerRailModel>(session, channelTreeModel.get());
    typingTracker = std::make_unique<Core::TypingTracker>();
    memberListModel = std::make_unique<MemberListModel>(session->getImageManager());
    chatModel->setAvatarUrlResolver([this](const Discord::User &author, Snowflake guild) {
        return instance ? instance->users()->getAvatarUrl(author, guild, 64) : Discord::Cdn::userAvatar(author.id.get(), author.avatar.get(), 64);
    });
    chatModel->setDisplayNameResolver([this](const Discord::User &author, Snowflake guild) {
        return instance ? instance->users()->getAuthorDisplayName(author, guild) : std::string{};
    });
    chatModel->setRoleColorResolver([this](Snowflake user, Snowflake guild) { return roleColor(user, guild); });
    chatModel->setChannelNameResolver([this](Snowflake id) {
        if (instance) if (auto channel = instance->getChannel(id); channel && channel->name.hasValue()) return channel->name.get();
        return std::string{};
    });
    chatModel->setGuildInfoResolver([this](Snowflake id) -> std::pair<std::string, std::string> {
        if (!instance) return {};
        auto guild = instance->getGuild(id);
        if (!guild || !guild->name.hasValue()) return {};
        return {guild->name.get(), guild->icon.hasValue() && !guild->icon.get().empty() ? Discord::Cdn::guildIcon(id, guild->icon.get(), 64) : std::string{}};
    });
    track(session->ready.connect([this](const Discord::Ready &ready) {
        channelTreeModel->populateFromReady(ready);
        auto saved = Snowflake(Platform::Settings::instance().get<uint64_t>("ui/rail/lastAccountId", 0));
        if (!current.accountId.isValid() || saved == ready.user->id) restoreLastLocation(ready.user->id);
        accountReady.publish(ready.user->id);
    }));
    track(accountsModel->changed.connect([this](int row) {
        auto *account = accountsModel->accountAt(row);
        if (!account) return;
        if (account->state == Core::ConnectionState::Connected) {
            channelTreeModel->addAccount(*account);
            setupPermanentConnections(coreSession->client(account->id));
        } else if (account->state == Core::ConnectionState::Disconnected) {
            if (instance && instance->accountId() == account->id) switchActiveInstance(nullptr);
            channelTreeModel->removeAccount(account->id);
            connectedInstances.erase(account->id);
            callSounds->forget(account->id);
        }
    }));
    callSounds = std::make_unique<CallSounds>();
    for (auto *client : session->getClients()) setupPermanentConnections(client);
    setWindowTitle("Styx");
    resize(1280, 800);
    restoreWindowState();
    applyCloseToTray();
    closeRequested = [this] {
        saveWindowState();
        if (trayIcon && trayIcon->available()) { hide(); return false; }
        return true;
    };
    contextMenu = std::make_unique<Rml::ContextMenu>(*this);
    track(locationChanged.connect([this](const Location &) { contextMenu->hide(); }));
    track(instanceChanged.connect([this](Core::ClientInstance *) { contextMenu->hide(); }));
    track(documentLoaded.connect([this](::Rml::ElementDocument *doc) { onDocumentLoaded(doc); }));
    loadDocument("main.rml");
}
Shell::~Shell()
{
    saveWindowState();
    activeConnections.clear();
    coreSession->setCaptchaResolver(nullptr);
    Rml::Runtime::instance().setImageManager(nullptr);
    if (primary == this) primary = nullptr;
}
Core::ImageManager *Shell::images() const { return coreSession->getImageManager(); }
ClientInstance *Shell::instanceFor(Snowflake id) const { return coreSession->client(id); }
std::string Shell::imageSource(const std::string &url, Platform::Size size) const { return Rml::Runtime::instance().imageSource(url, size, current.accountId); }
void Shell::onDocumentLoaded(::Rml::ElementDocument *doc)
{
    controllers.clear();
    auto region = [doc](const char *id) { return doc->GetElementById(id); };
    moderationController = std::make_unique<MemberModeration::Controller>(*this);
    controllers.push_back(std::make_unique<Rml::RailController>(*this, region("rail")));
    controllers.push_back(std::make_unique<Rml::ChannelListController>(*this, region("sidebar")));
    controllers.push_back(std::make_unique<Rml::UserPanelController>(*this, region("sidebar")));
    controllers.push_back(std::make_unique<Rml::ChatController>(*this, region("main")));
    controllers.push_back(std::make_unique<Rml::MemberListController>(*this, region("members")));
#ifndef STYX_NO_VOICE
    controllers.push_back(std::make_unique<Rml::CallController>(*this, region("call")));
#endif
    controllers.push_back(std::make_unique<Rml::FriendsController>(*this, region("friends")));
    controllers.push_back(std::make_unique<Rml::SettingsController>(*this, region("settings-overlay")));
    auto confirm = std::make_unique<Rml::ConfirmModalController>(*this);
    confirmModalController = confirm.get(); controllers.push_back(std::move(confirm));
    auto profile = std::make_unique<Rml::UserProfileController>(*this);
    userProfileController = profile.get(); controllers.push_back(std::move(profile));
    auto viewer = std::make_unique<Rml::ImageViewerController>(*this);
    imageViewerController = viewer.get(); controllers.push_back(std::move(viewer));
    auto picker = std::make_unique<Rml::ExpressionPickerController>(*this);
    expressionPickerController = picker.get(); controllers.push_back(std::move(picker));
    accountsController = std::make_unique<Rml::AccountsController>(*this);
    if (accountsModel->rowCount() == 0) accountsController->showLogin();
    applyBodyClasses();
}
void Shell::applyBodyClasses()
{
    if (auto *doc = document()) {
        doc->SetClass("call-open", callOpen);
        doc->SetClass("members-hidden", !memberListVisible);
        doc->SetClass("friends-open", isFriendsOpen());
        doc->SetClass("settings-open", settingsOpen);
    }
    if (forumController && forumController->isOpen()) forumController->syncFolderState();
    requestRender(200);
}
void Shell::setCallOpen(bool open) { if (callOpen == open) return; callOpen = open; applyBodyClasses(); callOpenChanged.publish(open); }
void Shell::setMemberListVisible(bool visible) { if (memberListVisible == visible) return; memberListVisible = visible; applyBodyClasses(); memberListVisibleChanged.publish(visible); }
void Shell::showFromTray() { if (!isMaximized()) showNormal(); else show(); raise(); activateWindow(); }
void Shell::quitFromTray() { saveWindowState(); App::quit(); }
void Shell::applyCloseToTray()
{
    bool wanted = Platform::Settings::instance().getBool("general/close_to_tray", false);
    if (wanted == bool(trayIcon)) return;
    if (!wanted) { if (!isVisible()) showFromTray(); trayIcon.reset(); return; }
    trayIcon = std::make_unique<TrayIcon>();
    trayIcon->setToolTip(windowTitle());
    track(trayIcon->showWindowRequested.connect([this] { showFromTray(); }));
    track(trayIcon->quitRequested.connect([this] { quitFromTray(); }));
}
void Shell::refreshTaskbarBadge()
{
    namespace Settings = Core::Settings;
    const bool shown = Settings::getBool(Settings::Keys::TaskbarBadge, Settings::Defaults::TaskbarBadge);
    Platform::Badge::setCount(nativeHandle(), shown ? mentionTotal : 0);
}
void Shell::saveWindowState()
{
    auto &settings = Platform::Settings::instance();
    auto pos = position();
    settings.setJson("layout/nativeGeometry", {{"x", int(pos.x)}, {"y", int(pos.y)}, {"width", width()}, {"height", height()}, {"maximized", isMaximized()}});
    if (serverRailModel) settings.set("ui/rail/expandedFolders", serverRailModel->expandedFolderKeys());
    nlohmann::json channels = nlohmann::json::object();
    for (const auto &[key, value] : lastViewedChannel) channels[key] = uint64_t(value);
    settings.setJson("ui/rail/lastChannels", channels);
    settings.save();
}
void Shell::restoreWindowState()
{
    auto &settings = Platform::Settings::instance();
    auto geometry = settings.getJson("layout/nativeGeometry");
    if (geometry.is_object()) {
        resize(std::max(400, geometry.value("width", 1280)), std::max(300, geometry.value("height", 800)));
        move(geometry.value("x", 100), geometry.value("y", 100));
        if (geometry.value("maximized", false)) showMaximized();
    }
    serverRailModel->setExpandedFolderKeys(settings.get<std::vector<std::string>>("ui/rail/expandedFolders", {}));
    auto channels = settings.getJson("ui/rail/lastChannels");
    if (channels.is_object()) for (auto it = channels.begin(); it != channels.end(); ++it) {
        if (it.value().is_number_unsigned() || it.value().is_number_integer()) lastViewedChannel.emplace(it.key(), Snowflake(it.value().get<uint64_t>()));
    }
    const auto count = settings.getInt("ui/rail/lastChannels/size", 0);
    for (int i = 1; i <= count; ++i) {
        auto prefix = "ui/rail/lastChannels/" + std::to_string(i) + "/";
        auto key = settings.getString(prefix + "key");
        if (!key.empty()) lastViewedChannel.try_emplace(key, Snowflake(settings.get<uint64_t>(prefix + "channel", 0)));
    }
}
void Shell::restoreLastLocation(Snowflake account)
{
    auto &settings = Platform::Settings::instance();
    auto saved = Snowflake(settings.get<uint64_t>("ui/rail/lastAccountId", 0));
    if (saved.isValid() && saved != account) return;
    auto guild = Snowflake(settings.get<uint64_t>("ui/rail/lastGuildId", 0));
    if (!settings.getBool("ui/rail/lastIsHome", true) && guild.isValid() && channelTreeModel->serverNode(account, guild)) openGuild(account, guild);
    else openHome(account);
}
bool Shell::channelReadable(Snowflake account, Snowflake guild, Snowflake channel) const
{
    auto *node = channelTreeModel->findChannelTreeNode(channel, account);
    if (!node || (node->type != ChannelNode::Type::Channel && node->type != ChannelNode::Type::VoiceChannel)) return false;
    auto *server = ChannelTreeModel::findGuildNode(node);
    if (!server || server->id != guild) return false;
    auto *client = instanceFor(account);
    return !client || !client->permissions() || client->permissions()->hasChannelPermission(account, channel, Discord::Permission::VIEW_CHANNEL);
}
Snowflake Shell::firstReadableChannel(Snowflake account, Snowflake guild) const
{
    auto *node = channelTreeModel->serverNode(account, guild);
    if (!node) return {};
    auto *client = instanceFor(account);
    auto *permissions = client ? client->permissions() : nullptr;
    auto readable = [&](Snowflake id) { return !permissions || permissions->hasChannelPermission(account, id, Discord::Permission::VIEW_CHANNEL); };
    for (const auto &child : node->children) {
        if (child->type == ChannelNode::Type::Category) break;
        if (child->type == ChannelNode::Type::Channel && readable(child->id)) return child->id;
    }
    for (const auto &category : node->children) if (category->type == ChannelNode::Type::Category)
        for (const auto &child : category->children) if (child->type == ChannelNode::Type::Channel && readable(child->id)) return child->id;
    return {};
}
void Shell::setupPermanentConnections(ClientInstance *client)
{
    if (!client || !connectedInstances.insert(client->accountId()).second) return;
    callSounds->watch(client);
    track(client->guildCreated.connect([this, client](const Discord::GatewayGuild &guild) { channelTreeModel->addGuild(guild, client->accountId()); }));
    track(client->guildUpdated.connect([this, client](const Discord::Guild &guild) { channelTreeModel->updateGuild(guild, client->accountId()); }));
    track(client->guildRemoved.connect([this, client](Snowflake guild) { channelTreeModel->removeGuild(client->accountId(), guild); }));
    track(client->discord()->guildLeaveFailed.connect([this, client](Snowflake id, const std::string &error) {
        auto guild = client->getGuild(id);
        std::string name = guild ? guild->name.get() : id.toString();
        Rml::ConfirmOptions options;
        options.title = "Failed to leave server";
        options.body = "Could not leave <b>" + Platform::htmlEscape(name) + "</b>.<br/>Error: " + Platform::htmlEscape(error);
        options.confirmText = "OK"; options.cancelText.clear();
        Rml::showConfirmModal(*this, options);
    }));
    track(client->channelCreated.connect([this, client](const Discord::ChannelCreate &event) { channelTreeModel->addChannel(event, client->accountId()); }));
    track(client->channelUpdated.connect([this, client](const Discord::ChannelUpdate &event) { channelTreeModel->updateChannel(event, client->accountId()); }));
    track(client->channelDeleted.connect([this, client](const Discord::ChannelDelete &event) { channelTreeModel->deleteChannel(event, client->accountId()); }));
    track(client->threadCreated.connect([this, client](const Discord::Channel &event) { channelTreeModel->addThread(event, client->accountId()); }));
    track(client->threadUpdated.connect([this, client](const Discord::Channel &event) { channelTreeModel->updateThread(event, client->accountId()); }));
    track(client->threadDeleted.connect([this, client](Snowflake id, Snowflake, Snowflake) { channelTreeModel->removeThread(id, client->accountId()); }));
    track(client->threadListSynced.connect([this, client](Snowflake guild, const std::vector<Snowflake> &parents, const std::vector<Discord::Channel> &threads) { channelTreeModel->syncThreads(guild, parents, threads, client->accountId()); }));
    track(client->threadMembershipChanged.connect([this, client](Snowflake id) {
        auto account = client->accountId();
        if (client->isThreadJoined(id)) {
            if (channelTreeModel->findChannelTreeNode(id, account)) channelTreeModel->promoteTemporaryThread(id);
            else if (auto channel = client->getChannel(id)) channelTreeModel->addThread(*channel, account);
            channelTreeModel->updateReadState(id, account);
        } else if (chatModel->getActiveChannelId() == id) {
            if (auto channel = client->getChannel(id)) channelTreeModel->showTemporaryThread(*channel, account);
        } else channelTreeModel->removeThread(id, account);
    }));
    auto rolesChanged = [this](const auto &event) {
        if (event.guildId.hasValue()) { channelTreeModel->invalidateGuildData(event.guildId.get()); roleColorCache.clear(); chatModel->refreshUsersInView({}); }
    };
    track(client->guildRoleCreated.connect(rolesChanged));
    track(client->guildRoleUpdated.connect(rolesChanged));
    track(client->guildRoleDeleted.connect(rolesChanged));
    track(client->readStateChanged.connect([this, client](Snowflake channel) { channelTreeModel->updateReadState(channel, client->accountId()); }));
    track(client->notificationRequested.connect([this, client](const Discord::Message &msg, bool mentioned) {
        namespace Settings = Core::Settings;
        if (Settings::getBool(Settings::Keys::DesktopNotifications, Settings::Defaults::DesktopNotifications))
            showMessageNotification(this, client, msg);
        if (!Platform::Desktop::applicationActive() && Settings::getBool(Settings::Keys::FlashTaskbar, Settings::Defaults::FlashTaskbar))
            requestAttention();
        Core::Sounds::play(mentioned ? Core::Sounds::Sound::Mention : Core::Sounds::Sound::Message);
    }));
    track(client->presences()->presencesChanged.connect([this, client](const std::vector<Snowflake> &users) { channelTreeModel->updatePresence(client->accountId(), users); }));
    track(client->forumBadgeChanged.connect([this, client](Snowflake id) { channelTreeModel->updateForumBadge(id, client->accountId()); }));
    track(client->forumJoinedPostsChanged.connect([this, client](Snowflake id) { channelTreeModel->updateForumThreads(id, client->accountId()); }));
    track(client->channelLastMessageUpdated.connect([this, client](Snowflake channel, Snowflake message) { channelTreeModel->updateChannelLastMessageId(channel, message, client->accountId()); }));
    track(client->guildSettingsChanged.connect([this, client](Snowflake guild) { channelTreeModel->updateGuildSettings(guild, client->accountId()); }));
    track(client->voiceStateChanged.connect([this, client](Snowflake channel, Snowflake) { channelTreeModel->setAccountVoiceChannel(client->accountId(), channel); }));
#ifndef STYX_NO_VOICE
    if (client->voice()) {
        track(client->voice()->channelVoiceMemberChanged.connect([this, client](Snowflake channel, Snowflake user, bool joined) {
            channelTreeModel->updateVoiceCount(channel, client->voice()->channelVoiceUserCount(channel), client->accountId());
            channelTreeModel->updateVoiceParticipant(channel, user, joined, client->accountId());
        }));
        track(client->voice()->participantVoiceStateChanged.connect([this, client](Snowflake channel, Snowflake user) {
            channelTreeModel->updateVoiceParticipantState(channel, user, client->accountId());
            if (user == client->accountId()) if (auto *participant = client->voice()->participant(user)) {
                if (participant->selfMute != selfMute) { selfMute = participant->selfMute; selfMuteChanged.publish(selfMute); }
                if (participant->selfDeaf != selfDeaf) { selfDeaf = participant->selfDeaf; selfDeafChanged.publish(selfDeaf); }
            }
        }));
    }
#endif
}
void Shell::switchActiveInstance(ClientInstance *next)
{
    if (instance == next) return;
    activeConnections.clear();
    memberListModel->setManager(nullptr);
    memberListModel->setPresenceManager(nullptr);
    typingTracker->clear();
    typingTracker->setUserManager(nullptr);
    instance = next;
    if (!instance) { instanceChanged.publish(nullptr); return; }
    memberListModel->setAccount(instance->accountId());
    memberListModel->setPresenceManager(instance->presences());
    memberListModel->setManager(instance->memberList());
    chatModel->setAccount(instance->accountId());
    typingTracker->setUserManager(instance->users());
    typingTracker->setCurrentUserId(instance->accountId());
    auto *messages = instance->messages();
    activeConnections.push_back(messages->messagesReceived.connect([this](const Core::MessageRequestResult &result) { chatModel->handleIncomingMessages(result); }));
    activeConnections.push_back(messages->messageErrored.connect([this](const std::string &nonce) { chatModel->handleMessageErrored(nonce); }));
    activeConnections.push_back(messages->messageDeleted.connect([this](Snowflake channel, Snowflake message) { chatModel->handleMessageDeleted(channel, message); }));
    activeConnections.push_back(messages->attachmentUploadProgress.connect([this](const std::string &nonce, int index, int64_t sent, int64_t total) { chatModel->handleUploadProgress(nonce, index, sent, total); }));
    activeConnections.push_back(instance->discord()->typingStart.connect([this](const Discord::TypingStart &event) {
        if (event.member.hasValue() && event.guildId.hasValue()) instance->users()->saveMemberWithUser(event.guildId.get(), event.member.get());
        typingTracker->addTyper(event.channelId, event.userId, event.guildId.hasValue() ? std::optional<Snowflake>(event.guildId.get()) : std::nullopt);
    }));
    activeConnections.push_back(instance->discord()->messageCreated.connect([this](const Discord::Message &message) { if (message.author.hasValue()) typingTracker->removeTyper(message.channelId, message.author->id); }));
    activeConnections.push_back(instance->membersUpdated.connect([this](Snowflake, const std::vector<Snowflake> &users) {
        for (auto user : users) roleColorCache.erase(user);
        chatModel->refreshUsersInView(users);
    }));
    activeConnections.push_back(instance->relationships()->relationshipChanged.connect([this](Snowflake user) { chatModel->refreshUsersInView({user}); }));
    instanceChanged.publish(instance);
}
Platform::Color Shell::roleColor(Snowflake user, Snowflake guild)
{
    if (!instance || !guild.isValid()) return {};
    if (roleColorGuild != guild) { roleColorGuild = guild; roleColorCache.clear(); }
    else if (auto it = roleColorCache.find(user); it != roleColorCache.end()) return it->second;
    Platform::Color color;
    for (const auto &role : instance->getMemberRolesSorted(guild, user)) if (role.hasColor()) { color = role.getColor(); break; }
    roleColorCache[user] = color;
    return color;
}
void Shell::openFriends(Snowflake account)
{
    auto *client = instanceFor(account);
    if (!client) return;
    switchActiveInstance(client);
    serverRailModel->setSelected(ServerRailModel::Kind::AccountHome, account, account);
    current = {account, Snowflake::Invalid, Snowflake::Invalid};
    channelFilterProxy->setSelectedChannel(Snowflake::Invalid, account);
    chatModel->setActiveChannel(Snowflake::Invalid, Snowflake::Invalid);
    client->noteChannelSelected(Snowflake::Invalid);
    if (client->memberList()) client->memberList()->clear();
    if (forumController) forumController->close();
    applyBodyClasses(); locationChanged.publish(current);
    auto &settings = Platform::Settings::instance();
    settings.set("ui/rail/lastAccountId", uint64_t(account)); settings.setBool("ui/rail/lastIsHome", true);
}
bool Shell::isFriendsOpen() const { return current.isHome() && !current.channelId.isValid(); }
void Shell::openHome(Snowflake account) { openFriends(account); }
void Shell::openGuild(Snowflake account, Snowflake guild)
{
    auto *client = instanceFor(account);
    if (!client) return;
    switchActiveInstance(client);
    serverRailModel->setSelected(ServerRailModel::Kind::Server, account, guild);
    Snowflake channel;
    auto it = lastViewedChannel.find(guildChannelKey(account, guild));
    if (it != lastViewedChannel.end() && channelReadable(account, guild, it->second)) channel = it->second;
    else {
        auto *node = channelTreeModel->serverNode(account, guild);
        channel = node && channelReadable(account, guild, node->rulesChannelId) ? node->rulesChannelId : firstReadableChannel(account, guild);
    }
    if (channel.isValid()) openChannel(account, guild, channel);
    else {
        current = {account, guild, Snowflake::Invalid};
        channelFilterProxy->setSelectedChannel(Snowflake::Invalid, account);
        chatModel->setActiveChannel(Snowflake::Invalid, guild);
        client->noteChannelSelected(Snowflake::Invalid);
        applyBodyClasses(); locationChanged.publish(current);
    }
    auto &settings = Platform::Settings::instance();
    settings.set("ui/rail/lastAccountId", uint64_t(account)); settings.set("ui/rail/lastGuildId", uint64_t(guild)); settings.setBool("ui/rail/lastIsHome", false);
}
void Shell::openChannel(Snowflake account, Snowflake guild, Snowflake channel)
{
    auto *client = instanceFor(account);
    if (!client) return;
    switchActiveInstance(client);
    auto *node = channelTreeModel->findChannelTreeNode(channel, account);
    if (!node) if (auto data = client->getChannel(channel); data && data->isThread()) {
        channelTreeModel->showTemporaryThread(*data, account); node = channelTreeModel->findChannelTreeNode(channel, account);
    }
    if (!guild.isValid() && node) if (auto *server = ChannelTreeModel::findGuildNode(node)) guild = server->id;
    auto *forum = node && node->type == ChannelNode::Type::Thread ? node->parent : node;
    if (forum && forum->type == ChannelNode::Type::Forum && (!node || node->type != ChannelNode::Type::Thread)) { openForum(account, guild, forum->id); return; }
    if (forumController) forumController->close();
    bool voice = node && node->type == ChannelNode::Type::VoiceChannel;
    if (voice) joinVoice(account, guild, channel);
    setCallOpen(voice);
    channelFilterProxy->setSelectedChannel(channel, account);
    channelTreeModel->clearTemporaryThread(channel);
    client->readState()->setActiveChannel(channel);
    client->noteChannelSelected(channel);
    bool dm = node ? node->type == ChannelNode::Type::DMChannel : !guild.isValid();
    if (dm) { if (client->memberList()) client->memberList()->clear(); }
    else if (guild.isValid()) {
        Snowflake parent;
        if (auto data = client->getChannel(channel); data && data->isThread()) parent = data->parentId.valueOr(Snowflake::Invalid);
        client->memberList()->setActiveChannel(guild, parent.isValid() ? parent : channel);
    }
    if (roleColorGuild != guild) { roleColorGuild = guild; roleColorCache.clear(); }
    chatModel->setActiveChannel(channel, guild);
    typingTracker->setActiveChannel(channel);
    client->messages()->requestLoadChannel(channel);
    lastViewedChannel[guildChannelKey(account, guild)] = channel;
    current = {account, guild, channel};
    applyBodyClasses(); locationChanged.publish(current);
}
void Shell::openDmWithUser(Snowflake account, Snowflake user)
{
    auto *client = instanceFor(account);
    if (!client) return;
    Platform::Guard<Shell> guard(this);
    client->openDmChannel(user, [guard, account](const Core::Result<Snowflake> &result) {
        if (guard && result.success()) guard->openChannel(account, Snowflake::Invalid, *result.value);
    });
}
void Shell::jumpToMessage(Snowflake channel, Snowflake message)
{
    if (!instance) return;
    if (chatModel->getActiveChannelId() != channel) {
        auto account = instance->accountId();
        auto *node = channelTreeModel->findChannelTreeNode(channel, account);
        if (!node) {
            if (auto data = instance->getChannel(channel); data && data->isThread()) channelTreeModel->showTemporaryThread(*data, account);
            node = channelTreeModel->findChannelTreeNode(channel, account);
        }
        auto *guild = node ? ChannelTreeModel::findGuildNode(node) : nullptr;
        openChannel(account, guild ? guild->id : Snowflake::Invalid, channel);
    }
    instance->messages()->requestLoadAround(channel, message);
}
void Shell::joinVoice(Snowflake account, Snowflake guild, Snowflake channel)
{
    auto *client = instanceFor(account);
    if (!client) return;
    switchActiveInstance(client);
#ifndef STYX_NO_VOICE
    if (!client->discord()->getProxy().canRelayUdp()) {
        Rml::ConfirmOptions options;
        options.title = "Voice unavailable";
        options.body = "This account uses an HTTP proxy, which cannot carry voice traffic.<br/>Switch it to a SOCKS5 proxy to use voice.";
        options.confirmText = "OK"; options.cancelText.clear();
        Rml::showConfirmModal(*this, options); return;
    }
    client->discord()->sendVoiceStateUpdate(guild, channel, selfMute, selfDeaf);
#endif
}
void Shell::leaveVoice()
{
    if (!instance) return;
#ifndef STYX_NO_VOICE
    if (instance->isInVoice()) instance->discord()->sendVoiceStateUpdate(instance->voiceGuildId(), Snowflake::Invalid, false, false);
#endif
    setCallOpen(false);
}
void Shell::setSelfMute(bool muted)
{
    selfMute = muted;
    if (instance && instance->isInVoice()) instance->discord()->sendVoiceStateUpdate(instance->voiceGuildId(), instance->voiceChannelId(), selfMute, selfDeaf);
    selfMuteChanged.publish(muted);
}
void Shell::setSelfDeaf(bool deafened)
{
    selfDeaf = deafened;
    if (instance && instance->isInVoice()) instance->discord()->sendVoiceStateUpdate(instance->voiceGuildId(), instance->voiceChannelId(), selfMute, selfDeaf);
    selfDeafChanged.publish(deafened);
}
bool Shell::sendMessage(const std::string &text, Snowflake reply) { return sendMessage(text, {}, reply); }
bool Shell::sendMessage(const std::string &text, const std::vector<Core::PendingAttachment> &attachments, Snowflake reply)
{
    if (!instance || !chatModel->getActiveChannelId().isValid()) return false;
    instance->messages()->sendMessage(chatModel->getActiveChannelId(), text, reply, attachments);
    return true;
}
void Shell::markChannelRead(Snowflake channel)
{
    if (!channel.isValid()) return;
    auto account = current.accountId.isValid() ? current.accountId : instance ? instance->accountId() : Snowflake::Invalid;
    auto *client = instanceFor(account);
    if (!client) return;
    if (auto *node = channelTreeModel->findChannelTreeNode(channel, account)) { client->readState()->markChannelsAsRead(channelTreeModel->getMarkableChannelIds(node)); return; }
    client->readState()->markChannelAsRead(channel, client->readState()->getChannelLastMessageId(channel));
}
void Shell::markGuildRead(Snowflake account, Snowflake guild) { if (auto *client = instanceFor(account)) client->markGuildsAsRead({guild}); }
void Shell::showUserProfile(Snowflake user, Snowflake guild, const Platform::Rect &anchor) { if (instance && userProfileController) userProfileController->showProfile(user, guild, anchor); }
void Shell::openSettings() { setSettingsOpen(true); }
void Shell::closeSettings() { setSettingsOpen(false); }
void Shell::setSettingsOpen(bool open) { if (settingsOpen == open) return; settingsOpen = open; applyBodyClasses(); settingsOpenChanged.publish(open); }
void Shell::openGoLivePicker(bool screens)
{
#ifdef STYX_HAVE_SCREENSHARE
    if (!goLiveController) goLiveController = std::make_unique<Rml::GoLiveController>(*this);
    goLiveController->open(screens);
#endif
}
void Shell::closeGoLivePicker()
{
#ifdef STYX_HAVE_SCREENSHARE
    if (goLiveController) goLiveController->close();
#endif
}
bool Shell::isGoLivePickerOpen() const
{
#ifdef STYX_HAVE_SCREENSHARE
    return goLiveController && goLiveController->isOpen();
#else
    return false;
#endif
}
void Shell::openExpressionPickerForChat(Snowflake account, Snowflake channel, const Platform::Rect &anchor)
{
    if (expressionPickerController && instance) expressionPickerController->openForChat(instance->emojis(), instance->favoriteGifs(), account, channel, Rml::ExpressionPickerController::Tab::Emoji, anchor);
}
void Shell::openExpressionPickerForReaction(Snowflake account, Snowflake channel, Snowflake message, const Platform::Rect &anchor)
{
    if (expressionPickerController && instance) expressionPickerController->openForReaction(instance->emojis(), account, channel, message, anchor);
}
void Shell::closeExpressionPicker() { if (expressionPickerController) expressionPickerController->close(); }
void Shell::openAccounts() { if (!accountsController) accountsController = std::make_unique<Rml::AccountsController>(*this); if (accountsModel->rowCount() == 0) accountsController->showLogin(); else accountsController->showAccounts(); }
void Shell::openLogin() { if (!accountsController) accountsController = std::make_unique<Rml::AccountsController>(*this); accountsController->showLogin(); }
void Shell::openGuildSettings(Snowflake account, Snowflake guild, GuildSettingsSection section)
{
    auto *client = instanceFor(account);
    if (!client || !GuildSettingsAccess::canOpen(client, guild)) return;
    if (!guildSettingsController) guildSettingsController = std::make_unique<Rml::GuildSettingsController>(*this);
    guildSettingsController->open(account, guild, section);
}
void Shell::openGuildSettings(Snowflake account, Snowflake guild) { openGuildSettings(account, guild, GuildSettingsSection::Profile); }
void Shell::closeGuildSettings() { if (guildSettingsController) guildSettingsController->close(); }
bool Shell::isGuildSettingsOpen() const { return guildSettingsController && guildSettingsController->isOpen(); }
void Shell::openImage(const std::string &url, const std::vector<std::string> &gallery) { if (imageViewerController) imageViewerController->openImage(url, gallery); }
void Shell::openForum(Snowflake account, Snowflake guild, Snowflake forum)
{
    auto *client = instanceFor(account);
    if (!client) return;
    switchActiveInstance(client);
    if (!forumController) forumController = std::make_unique<Rml::ForumController>(*this);
    current = {account, guild, forum};
    channelFilterProxy->setSelectedChannel(forum, account);
    auto newest = client->readState()->getChannelLastMessageId(forum);
    if (newest.isValid()) client->readState()->markChannelAsRead(forum, newest);
    client->readState()->setActiveChannel(forum);
    applyBodyClasses(); locationChanged.publish(current);
    forumController->open(account, guild, forum);
}
void Shell::openThreadBrowser(Snowflake channel)
{
    if (!instance || !channel.isValid()) return;
    if (!threadBrowserController) {
        threadBrowserController = std::make_unique<Rml::ThreadBrowserController>(*this);
        track(threadBrowserController->threadActivated.connect([this](Snowflake thread) { if (instance) openChannel(instance->accountId(), current.guildId, thread); }));
    }
    auto data = instance->getChannel(channel);
    threadBrowserController->open(instance, channel, data ? data->name.valueOr(std::string{}) : std::string{});
}
void Shell::openSearch(Snowflake channel, const std::string &scopeName)
{
    if (!instance) return;
    const Snowflake guild = current.isHome() ? Snowflake::Invalid : current.guildId;
    if (!guild.isValid() && !channel.isValid()) return;
    if (!searchController) {
        searchController = std::make_unique<Rml::SearchController>(*this);
        track(searchController->resultActivated.connect([this](Snowflake resultChannel, Snowflake message) { jumpToMessage(resultChannel, message); }));
    }
    searchController->open(instance, guild, channel, scopeName);
}
void Shell::showContextMenu(std::vector<Rml::MenuItem> items) { if (contextMenu) contextMenu->show(std::move(items), mapFromGlobal(Platform::Desktop::cursorPosition())); }
void Shell::showContextMenuForUser(Snowflake userId, Snowflake guild)
{
    std::vector<Rml::MenuItem> menu;
    auto *client = instance;
    Platform::Guard<ClientInstance> guard(client);
    if (client) {
        auto user = client->users()->getUser(userId);
        auto name = client->users()->getDisplayName(userId, guild);
        auto username = user ? user->username.valueOr(std::string{}) : std::string{};
        auto avatar = user ? client->users()->getAvatarUrl(*user, guild, 128) : Discord::Cdn::defaultUserAvatar(userId, 128);
        Rml::MenuItem header{name}; header.enabled = false;
        header.subtitle = username != name ? username : std::string{};
        header.imageSource = Rml::Runtime::instance().imageSource(avatar, {64, 64}, client->accountId());
        menu.push_back(std::move(header)); menu.push_back({{}, {}, false, false, true});
    }
    menu.push_back({"Profile", [this, userId, guild] { showUserProfile(userId, guild); }});
    menu.push_back({"Mention", [userId] { Platform::Desktop::setClipboardText("<@" + userId.toString() + ">"); }, client != nullptr});
    menu.push_back({"Open DM", [this, guard, userId] { if (guard) openDmWithUser(guard->accountId(), userId); }, client && userId != client->accountId()});
    if (guild.isValid() && client) {
        auto target = MemberModeration::targetFor(client, guild, userId);
        if (!target || !MemberModeration::canEditRoles(client, *target)) {
            Rml::MenuItem roles{"Roles"};
            for (const auto &role : client->getMemberRolesSorted(guild, userId)) {
                Rml::MenuItem item{role.name.get()}; item.enabled = false;
                if (role.hasColor()) item.color = role.getColor().css();
                roles.children.push_back(std::move(item));
            }
            if (roles.children.empty()) roles.children.push_back({"No roles", {}, false});
            menu.push_back(std::move(roles));
        }
        if (target) MemberModeration::addModerationActions(menu, *this, client, *target);
    }
    menu.push_back({{}, {}, false, false, true});
    menu.push_back({"Copy User ID", [userId] { Platform::Desktop::setClipboardText(userId.toString()); }});
    showContextMenu(std::move(menu));
}
void Shell::showContextMenuForGuild(Snowflake account, Snowflake guild)
{
    auto *client = instanceFor(account);
    if (!client) return;
    Platform::Guard<ClientInstance> guard(client);
    std::vector<Rml::MenuItem> menu{{"Mark As Read", [this, account, guild] { markGuildRead(account, guild); }}};
    Rml::MenuItem settings{"Server Settings"};
    for (auto section : GuildSettingsAccess::visibleSections(client, guild)) if (section != GuildSettingsSection::DeleteServer)
        settings.children.push_back({GuildSettingsAccess::title(section), [this, account, guild, section] { openGuildSettings(account, guild, section); }});
    if (!settings.children.empty()) { menu.push_back({{}, {}, false, false, true}); menu.push_back(std::move(settings)); }
    menu.push_back({{}, {}, false, false, true});
    auto data = client->getGuild(guild);
    bool owner = data && data->ownerId.hasValue() && data->ownerId.get() == account;
    menu.push_back({"Leave Server", [guard, guild] { if (guard) guard->discord()->leaveGuild(guild); }, !owner, true});
    menu.push_back({{}, {}, false, false, true});
    menu.push_back({"Copy Server ID", [guild] { Platform::Desktop::setClipboardText(guild.toString()); }});
    showContextMenu(std::move(menu));
}
void Shell::showContextMenuForChannel(Snowflake account, Snowflake guild, Snowflake channel)
{
    auto *client = instanceFor(account);
    if (!client) return;
    std::vector<Rml::MenuItem> menu;
    auto *node = channelTreeModel->findChannelTreeNode(channel, account);
    bool voice = node && node->type == ChannelNode::Type::VoiceChannel;
    bool dm = node && node->type == ChannelNode::Type::DMChannel;
    if (voice || dm) {
        bool join = false, disconnect = false;
#ifndef STYX_NO_VOICE
        join = true; disconnect = client->isInVoice() && client->voiceChannelId() == channel;
#endif
        menu.push_back({dm ? "Join Call" : "Join Voice Channel", [this, account, guild, channel] { joinVoice(account, guild, channel); setCallOpen(true); }, join});
        menu.push_back({"Disconnect from Voice", [this] { leaveVoice(); }, disconnect});
        menu.push_back({{}, {}, false, false, true});
    }
    menu.push_back({"Mark As Read", [this, channel] { markChannelRead(channel); }});
    menu.push_back({{}, {}, false, false, true});
    menu.push_back({"Copy Channel ID", [channel] { Platform::Desktop::setClipboardText(channel.toString()); }});
    showContextMenu(std::move(menu));
}
}
