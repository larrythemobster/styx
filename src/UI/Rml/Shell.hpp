#pragma once

#include "RmlView.hpp"
#include "ContextMenu.hpp"
#include "Core/Snowflake.hpp"
#include "UI/ChannelList/Navigation.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rml { class ElementDocument; class Element; }
namespace Styx {
namespace Core { class Session; class ClientInstance; class ImageManager; class TypingTracker; struct PendingAttachment; }
namespace Discord { struct Ready; struct TypingStart; }
namespace UI {
class AccountsModel;
class ChannelTreeModel;
class ChannelFilterProxyModel;
class ServerRailModel;
class ChatModel;
class MemberListModel;
class TrayIcon;
class CallSounds;
class PushToTalk;
class BrowserCaptchaResolver;
enum class GuildSettingsSection;
namespace MemberModeration { class Controller; }
namespace Rml {
class GuildSettingsController;
class Controller;
class AccountsController;
class ConfirmModalController;
class UserProfileController;
class ImageViewerController;
class GoLiveController;
class ForumController;
class ThreadBrowserController;
class SearchController;
class ExpressionPickerController;
class ShortcutController;
class JoinServerController;
class CreateServerController;
class CreatePollController;
class CreateGroupDmController;
}
struct Location {
    Core::Snowflake accountId, guildId, channelId;
    bool isHome() const { return !guildId.isValid(); }
    bool operator==(const Location &) const = default;
};
class Shell : public Rml::View {
public:
    explicit Shell(Core::Session *session);
    ~Shell() override;
    static Shell *primaryShell();
    Core::Session *session() const { return coreSession; }
    Core::ImageManager *images() const;
    Core::ClientInstance *currentInstance() const { return instance; }
    Core::ClientInstance *instanceFor(Core::Snowflake accountId) const;
    const Location &location() const { return current; }
    Rml::View *view() const { return const_cast<Shell *>(this); }
    ChannelTreeModel *channelTree() const { return channelTreeModel.get(); }
    PushToTalk *pushToTalk() const { return pushToTalkKey.get(); }
    // In-app keyboard shortcuts; controllers register their handlers when they're created.
    Rml::ShortcutController *shortcuts() const { return shortcutController.get(); }
    ChannelFilterProxyModel *channelFilter() const { return channelFilterProxy.get(); }
    ServerRailModel *rail() const { return serverRailModel.get(); }
    ChatModel *chat() const { return chatModel.get(); }
    MemberListModel *members() const { return memberListModel.get(); }
    Core::TypingTracker *typing() const { return typingTracker.get(); }
    AccountsModel *accounts() const { return accountsModel.get(); }
    Rml::AccountsController *accountsUi() const { return accountsController.get(); }
    Rml::ConfirmModalController *confirmModal() const { return confirmModalController; }
    std::string imageSource(const std::string &url, Platform::Size size) const;
    Platform::Color roleColor(Core::Snowflake userId, Core::Snowflake guildId);
    void openHome(Core::Snowflake accountId);
    void openFriends(Core::Snowflake accountId);
    bool isFriendsOpen() const;
    void openGuild(Core::Snowflake accountId, Core::Snowflake guildId);
    void openChannel(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId);
    void openDmWithUser(Core::Snowflake accountId, Core::Snowflake userId);
    void jumpToMessage(Core::Snowflake channelId, Core::Snowflake messageId);
    void setCallOpen(bool open);
    bool isCallOpen() const { return callOpen; }
    void setMemberListVisible(bool visible);
    bool isMemberListVisible() const { return memberListVisible; }
    bool isSelfMute() const { return selfMute; }
    bool isSelfDeaf() const { return selfDeaf; }
    void joinVoice(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId);
    void leaveVoice();
    void setSelfMute(bool muted);
    void setSelfDeaf(bool deafened);
    bool sendMessage(const std::string &text, Core::Snowflake replyTo = Core::Snowflake::Invalid);
    bool sendMessage(const std::string &text, const std::vector<Core::PendingAttachment> &attachments, Core::Snowflake replyTo = Core::Snowflake::Invalid);
    void markChannelRead(Core::Snowflake channelId);
    void markGuildRead(Core::Snowflake accountId, Core::Snowflake guildId);
    void showUserProfile(Core::Snowflake userId, Core::Snowflake guildId, const Platform::Rect &anchor = {});
    void openSettings();
    void closeSettings();
    void setSettingsOpen(bool open);
    bool isSettingsOpen() const { return settingsOpen; }
    void openAccounts();
    void openLogin();
    void openGuildSettings(Core::Snowflake accountId, Core::Snowflake guildId, GuildSettingsSection section);
    void openGuildSettings(Core::Snowflake accountId, Core::Snowflake guildId);
    void closeGuildSettings();
    bool isGuildSettingsOpen() const;
    void openImage(const std::string &url, const std::vector<std::string> &gallery = {});
    Rml::ExpressionPickerController *expressionPicker() const { return expressionPickerController; }
    void openExpressionPickerForChat(Core::Snowflake accountId, Core::Snowflake channelId, const Platform::Rect &anchor);
    void openExpressionPickerForReaction(Core::Snowflake accountId, Core::Snowflake channelId, Core::Snowflake messageId, const Platform::Rect &anchor);
    void closeExpressionPicker();
    void openGoLivePicker(bool screensTab = false);
    void closeGoLivePicker();
    bool isGoLivePickerOpen() const;
    void showContextMenu(std::vector<Rml::MenuItem> items);
    void showContextMenuForUser(Core::Snowflake userId, Core::Snowflake guildId);
    void showContextMenuForGuild(Core::Snowflake accountId, Core::Snowflake guildId);
    void showContextMenuForChannel(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId);
    void openForum(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake forumId);
    void openThreadBrowser(Core::Snowflake channelId);
    // Message search for the current server, or for the DM channelId at home.
    void openSearch(Core::Snowflake channelId, const std::string &scopeName);
    // The Join a Server dialog, looking code up at once unless it's empty. location is where the
    // invite was opened, sent to Discord as context ("Join Guild", "Markdown Link").
    void openJoinServer(const std::string &code, const std::string &location);
    // The rail's "Add a Server": create one, with a way across to joining by invite.
    void openCreateServer();
    void openCreatePoll();
    void openCreateGroupDm(Core::Snowflake sourceChannelId = Core::Snowflake::Invalid);
    // Opens a server the account just joined or created, now if it's in the tree, otherwise
    // when the gateway delivers it. A later call replaces an earlier wait.
    void openGuildWhenReady(Core::Snowflake accountId, Core::Snowflake guildId);
    void applyCloseToTray();
    // Shows the current mention count on the taskbar, or clears it when the setting is off.
    void refreshTaskbarBadge();
    Platform::Signal<const Location &> locationChanged;
    Platform::Signal<Core::ClientInstance *> instanceChanged;
    Platform::Signal<bool> callOpenChanged, memberListVisibleChanged, selfMuteChanged, selfDeafChanged, settingsOpenChanged;
    Platform::Signal<Core::Snowflake> accountReady;
private:
    void onDocumentLoaded(::Rml::ElementDocument *document);
    void setupPermanentConnections(Core::ClientInstance *instance);
    void switchActiveInstance(Core::ClientInstance *instance);
    void applyBodyClasses();
    void saveWindowState();
    void restoreWindowState();
    void restoreLastLocation(Core::Snowflake accountId);
    bool channelReadable(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId) const;
    Core::Snowflake firstReadableChannel(Core::Snowflake accountId, Core::Snowflake guildId) const;
    // The current server's channels, or the DMs at home, in sidebar order; voice channels are
    // left out so an arrow key never joins a call.
    std::vector<NavigationEntry> channelNavigation() const;
    // Home (id Invalid) and the current account's servers in rail order.
    std::vector<NavigationEntry> serverNavigation() const;
    void stepChannel(int direction, bool unreadOnly);
    void stepServer(int direction);
    // An OK-only dialog for a request that failed; bodyRml is already escaped.
    void showActionFailed(const std::string &title, const std::string &bodyRml);
    void showFromTray();
    void quitFromTray();
    Core::Session *coreSession;
    Core::ClientInstance *instance = nullptr;
    Location current;
    bool callOpen = false, memberListVisible = true, selfMute = false, selfDeaf = false, settingsOpen = false;
    std::unique_ptr<ChannelTreeModel> channelTreeModel;
    std::unique_ptr<ChannelFilterProxyModel> channelFilterProxy;
    std::unique_ptr<ServerRailModel> serverRailModel;
    std::unique_ptr<ChatModel> chatModel;
    std::unique_ptr<MemberListModel> memberListModel;
    std::unique_ptr<Core::TypingTracker> typingTracker;
    std::unique_ptr<AccountsModel> accountsModel;
    std::unique_ptr<BrowserCaptchaResolver> captchaResolver;
    std::unique_ptr<Rml::ContextMenu> contextMenu;
    std::unique_ptr<MemberModeration::Controller> moderationController;
    std::vector<std::unique_ptr<Rml::Controller>> controllers;
    Rml::ConfirmModalController *confirmModalController = nullptr;
    Rml::UserProfileController *userProfileController = nullptr;
    Rml::ImageViewerController *imageViewerController = nullptr;
    Rml::ExpressionPickerController *expressionPickerController = nullptr;
    std::unordered_set<Core::Snowflake> connectedInstances;
    int mentionTotal = 0;
    std::unordered_map<std::string, Core::Snowflake> lastViewedChannel;
    std::unordered_map<Core::Snowflake, Platform::Color> roleColorCache;
    Core::Snowflake roleColorGuild;
    std::vector<Platform::Connection> activeConnections;
    Platform::Connection pendingGuildOpen;
    std::unique_ptr<Rml::AccountsController> accountsController;
#ifdef STYX_HAVE_SCREENSHARE
    std::unique_ptr<Rml::GoLiveController> goLiveController;
#endif
    std::unique_ptr<TrayIcon> trayIcon;
    std::unique_ptr<CallSounds> callSounds;
    std::unique_ptr<PushToTalk> pushToTalkKey;
    std::unique_ptr<Rml::GuildSettingsController> guildSettingsController;
    std::unique_ptr<Rml::ForumController> forumController;
    std::unique_ptr<Rml::ThreadBrowserController> threadBrowserController;
    std::unique_ptr<Rml::SearchController> searchController;
    std::unique_ptr<Rml::CreateServerController> createServerController;
    std::unique_ptr<Rml::CreatePollController> createPollController;
    std::unique_ptr<Rml::CreateGroupDmController> createGroupDmController;
    std::unique_ptr<Rml::JoinServerController> joinServerController;
    // Declared after the controllers so it, and the handlers pointing into them, goes first.
    std::unique_ptr<Rml::ShortcutController> shortcutController;
};
}
}
