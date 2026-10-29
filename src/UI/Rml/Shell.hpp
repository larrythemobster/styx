#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QString>

#include <memory>
#include <vector>

#include "Core/Snowflake.hpp"
#include "ContextMenu.hpp"

namespace Rml {
class ElementDocument;
class Element;
} // namespace Rml

namespace Styx {
namespace Core {
class Session;
class ClientInstance;
class ImageManager;
class TypingTracker;
struct PendingAttachment;
} // namespace Core
namespace Discord {
struct Ready;
struct TypingStart;
} // namespace Discord

namespace UI {

class AccountsModel;
class ChannelTreeModel;
class ChannelFilterProxyModel;
class ServerRailModel;
class ChatModel;
class MemberListModel;
class TrayIcon;
enum class GuildSettingsSection;
namespace MemberModeration {
class Controller;
}
namespace Rml {
class GuildSettingsController;
class View;
class Controller;
class AccountsController;
class ConfirmModalController;
class UserProfileController;
class ImageViewerController;
class GoLiveController;
class ForumController;
class ThreadBrowserController;
class ExpressionPickerController;
} // namespace Rml

// Where the user is: an account, a guild (Invalid = Direct Messages home) and a channel.
struct Location
{
    Core::Snowflake accountId;
    Core::Snowflake guildId;
    Core::Snowflake channelId;

    [[nodiscard]] bool isHome() const { return !guildId.isValid(); }
    bool operator==(const Location &) const = default;
};

// The main window: Discord's layout drawn with RmlUi (resources/ui/main.rml).
//
// Owns the data models the old Qt views used (they are plain QAbstractItemModels and keep
// all the Discord logic: ordering, folders, read state, permissions, message caching) and
// the navigation state; controllers render the regions and call back in here to navigate.
class Shell : public QMainWindow
{
    Q_OBJECT
public:
    explicit Shell(Core::Session *session, QWidget *parent = nullptr);
    ~Shell() override;

    [[nodiscard]] static Shell *primaryShell();

    // --- shared state for controllers ---------------------------------------------------
    [[nodiscard]] Core::Session *session() const { return coreSession; }
    [[nodiscard]] Core::ImageManager *images() const;
    [[nodiscard]] Core::ClientInstance *currentInstance() const { return instance; }
    [[nodiscard]] Core::ClientInstance *instanceFor(Core::Snowflake accountId) const;
    [[nodiscard]] const Location &location() const { return current; }
    [[nodiscard]] Rml::View *view() const { return rmlView; }
    [[nodiscard]] ::Rml::ElementDocument *document() const;

    [[nodiscard]] ChannelTreeModel *channelTree() const { return channelTreeModel; }
    [[nodiscard]] ChannelFilterProxyModel *channelFilter() const { return channelFilterProxy; }
    [[nodiscard]] ServerRailModel *rail() const { return serverRailModel; }
    [[nodiscard]] ChatModel *chat() const { return chatModel; }
    [[nodiscard]] MemberListModel *members() const { return memberListModel; }
    [[nodiscard]] Core::TypingTracker *typing() const { return typingTracker; }
    [[nodiscard]] AccountsModel *accounts() const { return accountsModel; }
    [[nodiscard]] Rml::AccountsController *accountsUi() const { return accountsController.get(); }

    // Remote image as an RmlUi texture source (see Rml::Runtime::imageSource).
    QString imageSource(const QUrl &url, const QSize &logicalSize) const;
    // Display colour of a member's highest coloured role, or invalid.
    QColor roleColor(Core::Snowflake userId, Core::Snowflake guildId);

    // --- navigation -------------------------------------------------------------------
    void openHome(Core::Snowflake accountId);
    void openFriends(Core::Snowflake accountId);
    [[nodiscard]] bool isFriendsOpen() const;
    // Opens the guild's last viewed channel, else its first readable text channel.
    void openGuild(Core::Snowflake accountId, Core::Snowflake guildId);
    void openChannel(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId);
    void openDmWithUser(Core::Snowflake accountId, Core::Snowflake userId);
    void jumpToMessage(Core::Snowflake channelId, Core::Snowflake messageId);

    // Shows the voice/Go Live call view in place of the chat (Discord's "call" screen).
    void setCallOpen(bool open);
    [[nodiscard]] bool isCallOpen() const { return callOpen; }
    void setMemberListVisible(bool visible);
    [[nodiscard]] bool isMemberListVisible() const { return memberListVisible; }
    [[nodiscard]] bool isSelfMute() const { return selfMute; }
    [[nodiscard]] bool isSelfDeaf() const { return selfDeaf; }
    // --- actions --------------------------------------------------------------------------
    void joinVoice(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId);
    void leaveVoice();
    void setSelfMute(bool muted);
    void setSelfDeaf(bool deafened);
    bool sendMessage(const QString &text, Core::Snowflake replyTo = Core::Snowflake::Invalid);
    bool sendMessage(const QString &text, const QList<Core::PendingAttachment> &attachments, Core::Snowflake replyTo = Core::Snowflake::Invalid);
    void markChannelRead(Core::Snowflake channelId);
    void markGuildRead(Core::Snowflake accountId, Core::Snowflake guildId);

    // --- RmlUi dialogs and menus -------------------------------------------------------
    void showUserProfile(Core::Snowflake userId, Core::Snowflake guildId, const QRectF &anchorRect = QRectF());
    void openSettings();
    void closeSettings();
    void setSettingsOpen(bool open);
    [[nodiscard]] bool isSettingsOpen() const { return settingsOpen; }
    void openAccounts();
    void openLogin();
    void openGuildSettings(Core::Snowflake accountId, Core::Snowflake guildId, GuildSettingsSection section);
    void openGuildSettings(Core::Snowflake accountId, Core::Snowflake guildId);
    void closeGuildSettings();
    [[nodiscard]] bool isGuildSettingsOpen() const;
    void openImage(const QUrl &url, const QList<QUrl> &gallery = {});
    [[nodiscard]] Rml::ExpressionPickerController *expressionPicker() const { return expressionPickerController; }
    void openExpressionPickerForChat(Core::Snowflake accountId, Core::Snowflake channelId, const QRect &anchor);
    void openExpressionPickerForReaction(Core::Snowflake accountId, Core::Snowflake channelId, Core::Snowflake messageId, const QRect &anchor);
    void closeExpressionPicker();
    void openGoLivePicker(bool screensTab = false);
    void closeGoLivePicker();
    [[nodiscard]] bool isGoLivePickerOpen() const;
    void showContextMenu(std::vector<Rml::MenuItem> items);
    void showContextMenuForUser(Core::Snowflake userId, Core::Snowflake guildId);
    void showContextMenuForGuild(Core::Snowflake accountId, Core::Snowflake guildId);
    void showContextMenuForChannel(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId);
    void openForum(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake forumId);
    void openThreadBrowser(Core::Snowflake channelId);
    void applyCloseToTray();
signals:
    void locationChanged(const Styx::UI::Location &location);
    void instanceChanged(Core::ClientInstance *instance);
    void callOpenChanged(bool open);
    void memberListVisibleChanged(bool visible);
    // A new account finished READY (models are populated).
    void accountReady(Core::Snowflake accountId);
    void selfMuteChanged(bool muted);
    void selfDeafChanged(bool deafened);
    void settingsOpenChanged(bool open);
protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void onDocumentLoaded(::Rml::ElementDocument *document);
    void setupPermanentConnections(Core::ClientInstance *instance);
    void switchActiveInstance(Core::ClientInstance *instance);
    void applyBodyClasses();
    void saveWindowState();
    void restoreWindowState();
    void restoreLastLocation(Core::Snowflake accountId);
    [[nodiscard]] bool channelReadable(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake channelId) const;
    [[nodiscard]] Core::Snowflake firstReadableChannel(Core::Snowflake accountId, Core::Snowflake guildId) const;
    void showFromTray();
    void quitFromTray();
    Core::Session *coreSession;
    Core::ClientInstance *instance = nullptr; // cleared when its account disconnects
    Location current;
    bool callOpen = false;
    bool memberListVisible = true;
    bool selfMute = false;
    bool selfDeaf = false;
    Rml::View *rmlView = nullptr;
    std::unique_ptr<Rml::ContextMenu> contextMenu;
    std::unique_ptr<MemberModeration::Controller> moderationController;
    std::vector<std::unique_ptr<Rml::Controller>> controllers;
    Rml::ConfirmModalController *confirmModalController = nullptr;
    Rml::UserProfileController *userProfileController = nullptr;
    Rml::ImageViewerController *imageViewerController = nullptr;

    Rml::ExpressionPickerController *expressionPickerController = nullptr;
    ChannelTreeModel *channelTreeModel = nullptr;
    ChannelFilterProxyModel *channelFilterProxy = nullptr;
    ServerRailModel *serverRailModel = nullptr;
    ChatModel *chatModel = nullptr;
    MemberListModel *memberListModel = nullptr;
    Core::TypingTracker *typingTracker = nullptr;
    AccountsModel *accountsModel = nullptr;

    QSet<Core::Snowflake> connectedInstances;
    QHash<QString, Core::Snowflake> lastViewedChannel; // "account:guild" -> channel
    QHash<Core::Snowflake, QColor> roleColorCache;
    Core::Snowflake roleColorGuild;

    std::unique_ptr<Rml::AccountsController> accountsController;
    std::unique_ptr<Rml::GoLiveController> goLiveController;
    bool settingsOpen = false;
    TrayIcon *trayIcon = nullptr;
    std::unique_ptr<Rml::GuildSettingsController> guildSettingsController;
    std::unique_ptr<Rml::ForumController> forumController;
    std::unique_ptr<Rml::ThreadBrowserController> threadBrowserController;
};

} // namespace UI
} // namespace Styx
