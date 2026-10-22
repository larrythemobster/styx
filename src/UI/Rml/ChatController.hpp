#pragma once

#include "Controller.hpp"
#include "Shell.hpp"
#include "Core/PendingAttachment.hpp"
#include "Core/Snowflake.hpp"

#include <QPointer>
#include <QString>
#include <QList>
#include <QUrl>

namespace Rml {
class Element;
class ElementFormControlTextArea;
} // namespace Rml

namespace Acheron {

namespace Discord {
struct Emoji;
}

namespace UI {

class ExpressionPicker;

namespace Rml {

class ChatController final : public Controller
{
    Q_OBJECT
public:
    ChatController(Shell &shell, ::Rml::Element *root, QObject *parent = nullptr);
    ~ChatController() override;

private slots:
    void onLocationChanged(const Location &location);
    void onInstanceChanged(Core::ClientInstance *instance);
    void onRowsInserted(const QModelIndex &parent, int first, int last);
    void onRowsRemoved(const QModelIndex &parent, int first, int last);
    void onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);
    void onModelReset();
    void onAtLatestChanged(bool atLatest);
    void onTypersChanged();

private:
    void initElements();
    void setupEventListeners();
    void setupModelConnections();

    // Rendering regions
    void updateHeader();
    void updateTyping();
    void updateComposer();
    void updateComposerPlaceholder();
    QString conversationName() const;
    void updateComposerReplyBar();
    void updateComposerAttachments();
    void updateJumpBar();

    // Message DOM helpers
    void renderAllMessages();
    QString buildMessageHtml(int row);
    void checkAndLoadOlder();
    void checkAndLoadNewer();
    void setReplyTarget(Core::Snowflake messageId, const QString &authorName, const QString &snippet);
    void clearReplyTarget();
    void setEditTarget(Core::Snowflake messageId, const QString &content);
    void clearEditTarget();
    void showMessageContextMenu(Core::Snowflake messageId);
    void openReactionPickerForMessage(Core::Snowflake messageId);
    void openEmojiPickerForComposer();

    ::Rml::Element *headerEl = nullptr;
    ::Rml::Element *messagesEl = nullptr;
    ::Rml::Element *typingEl = nullptr;
    ::Rml::Element *composerEl = nullptr;
    ::Rml::ElementFormControlTextArea *textareaEl = nullptr;

    Core::Snowflake currentChannelId;
    Core::Snowflake currentGuildId;
    Core::Snowflake currentAccountId;
    Core::Snowflake headerRecipientId;

    bool wasAtBottom = true;
    bool loadingHistory = false;
    bool hasSendPermission = true;
    int channelRateLimit = 0;

    // Composer state
    Core::Snowflake replyTargetId;
    QString replyAuthorName;
    QString replySnippet;
    Core::Snowflake editingMessageId;
    QList<Core::PendingAttachment> pendingAttachments;

    QList<QMetaObject::Connection> instanceConnections;
    QPointer<ExpressionPicker> expressionPicker;
};

} // namespace Rml
} // namespace UI
} // namespace Acheron