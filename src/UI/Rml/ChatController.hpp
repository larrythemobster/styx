#pragma once

#include "Controller.hpp"
#include "Shell.hpp"
#include "Core/PendingAttachment.hpp"
#include "Core/Snowflake.hpp"

#include <string>
#include <vector>
#include "Platform/Signal.hpp"

namespace Rml {
class Element;
class ElementFormControlTextArea;
} // namespace Rml

namespace Styx {

namespace Discord {
struct Emoji;
}

namespace UI {


namespace Rml {

class ChatController final : public Controller
{
public:
    ChatController(Shell &shell, ::Rml::Element *root);
    ~ChatController() override;

private:
    void onLocationChanged(const Location &location);
    void onInstanceChanged(Core::ClientInstance *instance);
    void onRowsInserted(int first, int last);
    void onRowsRemoved(int first, int last);
    void onDataChanged(int first, int last);
    void onModelReset();
    void onAtLatestChanged(bool atLatest);
    void onTypersChanged();

private:
    void initElements();
    void setupEventListeners();
    void setupModelConnections();

    // Rendering regions
    void updateHeader();
    // Message search for this server, or for this DM at home.
    void openSearch();
    // Scrolls the messages one page up (-1) or down (1); false when the chat isn't showing.
    bool scrollMessagesByPage(int direction);
    // Marks the open channel read; false when it's already read or the composer wants Escape.
    bool markCurrentChannelRead();
    [[nodiscard]] bool chatCovered() const;
    void updateTyping();
    void updateComposer();
    void updateComposerPlaceholder();
    std::string conversationName() const;
    void updateComposerReplyBar();
    void updateComposerAttachments();
    void updateJumpBar();

    // Message DOM helpers
    void renderAllMessages();
    std::string buildMessageHtml(int row);
    void checkAndLoadOlder();
    void checkAndLoadNewer();
    void setReplyTarget(Core::Snowflake messageId, const std::string &authorName, const std::string &snippet);
    void clearReplyTarget();
    void setEditTarget(Core::Snowflake messageId, const std::string &content);
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
    std::string replyAuthorName;
    std::string replySnippet;
    Core::Snowflake editingMessageId;
    std::vector<Core::PendingAttachment> pendingAttachments;

    std::vector<Platform::Connection> instanceConnections;
    Platform::Connection emojiConnection;
    Platform::Connection gifConnection;
};

} // namespace Rml
} // namespace UI
} // namespace Styx