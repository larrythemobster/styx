#include "ChatController.hpp"
#include "ChatMarkdown.hpp"
#include "RmlRuntime.hpp"
#include "Shell.hpp"
#include "ShortcutController.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Emoji/UnicodeEmojiIndex.hpp"
#include "Core/MessageManager.hpp"
#include "Core/PermissionManager.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Core/ReadStateManager.hpp"
#include "Core/Session.hpp"
#include "Core/TypingTracker.hpp"
#include "Core/UserManager.hpp"

#include "Discord/ChannelLink.hpp"
#include "Discord/Client.hpp"
#include "Discord/Entities.hpp"

#include "Discord/CdnUrls.hpp"
#include "UI/Chat/ChatModel.hpp"
#include "ConfirmModal.hpp"
#include "ExpressionPickerController.hpp"
#include "UI/ChannelList/ChannelNode.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include "Platform/Desktop.hpp"
#include "Platform/Utf8.hpp"
#include <charconv>
#include <filesystem>
#include <format>
#include <utility>

namespace Styx {
namespace UI {
namespace Rml {

namespace {

template<class T = uint64_t> T parseNumber(std::string_view text)
{
    T value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}
std::string formatGutterTimestamp(const Platform::DateTime &time)
{
    auto clock = time.format("%I:%M %p");
    if (!clock.empty() && clock.front() == '0') clock.erase(0, 1);
    return clock;
}
std::string formatTimestamp(const Platform::DateTime &time)
{
    const auto days = time.daysUntil(Platform::DateTime::now());
    return (days == 0 ? "Today at " : days == 1 ? "Yesterday at " : time.format("%m/%d/%Y") + " ") + formatGutterTimestamp(time);
}
std::string formatDateSeparator(const Platform::DateTime &time) { return time.format("%B %d, %Y"); }
std::string formatFileSize(int64_t bytes)
{
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::format("{:.1f} KB", bytes / 1024.0);
    return std::format("{:.2f} MB", bytes / (1024.0 * 1024.0));
}
Platform::Rect pickerAnchor()
{
    auto position = Platform::Desktop::cursorPosition();
    return {position.x, position.y, 1, 1};
}

std::string systemMessageIcon(Discord::MessageType type)
{
    switch (type) {
    case Discord::MessageType::USER_JOIN:
    case Discord::MessageType::RECIPIENT_ADD:
        return std::string("icon:arrow-right");
    case Discord::MessageType::RECIPIENT_REMOVE:
        return std::string("icon:arrow-left");
    case Discord::MessageType::CALL:
        return std::string("icon:phone");
    case Discord::MessageType::CHANNEL_PINNED_MESSAGE:
        return std::string("icon:pin");
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_1:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_2:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_3:
        return std::string("icon:gem");
    case Discord::MessageType::THREAD_CREATED:
    case Discord::MessageType::THREAD_STARTER_MESSAGE:
        return std::string("icon:messages-square");
    default:
        return std::string("icon:message-circle");
    }
}

std::string systemMessageClass(Discord::MessageType type)
{
    switch (type) {
    case Discord::MessageType::USER_JOIN:
    case Discord::MessageType::RECIPIENT_ADD:
        return std::string("join");
    case Discord::MessageType::RECIPIENT_REMOVE:
        return std::string("leave");
    case Discord::MessageType::CALL:
        return std::string("call");
    case Discord::MessageType::CHANNEL_PINNED_MESSAGE:
        return std::string("pin");
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_1:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_2:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_3:
        return std::string("boost");
    case Discord::MessageType::THREAD_CREATED:
    case Discord::MessageType::THREAD_STARTER_MESSAGE:
        return std::string("thread");
    default:
        return std::string("generic");
    }
}

} // namespace

ChatController::ChatController(Shell &shell, ::Rml::Element *root)
    : Controller(shell, root)
{
    initElements();
    setupEventListeners();
    setupModelConnections();
    if (auto *shortcuts = shell.shortcuts())
        shortcuts->setHandler(ShortcutAction::Search, [this] { openSearch(); });

    currentChannelId = shell.location().channelId;
    currentGuildId = shell.location().guildId;
    currentAccountId = shell.location().accountId;

    onInstanceChanged(shell.currentInstance());
}

ChatController::~ChatController() = default;

void ChatController::initElements()
{
    if (!regionRoot)
        return;

    headerEl = regionRoot->GetElementById("header");
    messagesEl = regionRoot->GetElementById("messages");
    typingEl = regionRoot->GetElementById("typing");
    composerEl = regionRoot->GetElementById("composer");

    if (!headerEl && shell.document())
        headerEl = shell.document()->GetElementById("header");
    if (!messagesEl && shell.document())
        messagesEl = shell.document()->GetElementById("messages");
    if (!typingEl && shell.document())
        typingEl = shell.document()->GetElementById("typing");
    if (!composerEl && shell.document())
        composerEl = shell.document()->GetElementById("composer");
}

void ChatController::setupEventListeners()
{
    if (!regionRoot)
        return;

    // Delegate click events on regionRoot
    listen("click", [this](::Rml::Event &event) {
        ::Rml::Element *target = event.GetTargetElement();
        if (!target)
            return;

        // Check for actions
        ::Rml::Element *actionEl = closest(target, "data-action");
        if (!actionEl) {
            // Check for direct href link
            ::Rml::Element *linkEl = closest(target, "href");
            if (linkEl) {
                std::string url = attribute(linkEl, "href");
                if (url.starts_with(std::string("styx://channel/"))) {
                    uint64_t chId = parseNumber(std::string_view(url).substr(15));
                    if (chId != 0)
                        shell.openChannel(currentAccountId, currentGuildId, Core::Snowflake(chId));
                    return;
                }
                if (auto link = Discord::ChannelLink::parse(url)) {
                    if (link->messageId.isValid())
                        shell.jumpToMessage(link->channelId, link->messageId);
                    else
                        shell.openChannel(currentAccountId, link->guildId, link->channelId);
                    return;
                }
                Rml::showConfirmModal(
                    shell,
                    std::string("External Link"),
                    "Are you sure you want to open <b>" + escape(url) + "</b>?",
                    std::string("Open Link"),
                    [url]() { Platform::Desktop::openUrl(url); },
                    nullptr,
                    false);
                return;
            }
            return;
        }

        std::string action = attribute(actionEl, "data-action");
        if (action == std::string("toggle-members")) {
            shell.setMemberListVisible(!shell.isMemberListVisible());
            updateHeader();
        } else if (action == std::string("open-threads")) {
            shell.openThreadBrowser(currentChannelId);
        } else if (action == std::string("open-search")) {
            openSearch();
        } else if (action == std::string("user-profile")) {
            uint64_t uid = parseNumber(attribute(actionEl, "data-user-id"));
            if (uid != 0) {
                Platform::Rect anchorRect(actionEl->GetAbsoluteLeft(), actionEl->GetAbsoluteTop(), actionEl->GetClientWidth(), actionEl->GetClientHeight());
                shell.showUserProfile(Core::Snowflake(uid), currentGuildId, anchorRect);
            }
        } else if (action == std::string("jump-to-message")) {
            uint64_t mid = parseNumber(attribute(actionEl, "data-message-id"));
            if (mid != 0)
                shell.jumpToMessage(currentChannelId, Core::Snowflake(mid));
        } else if (action == std::string("jump-to-present")) {
            Core::ClientInstance *instance = shell.currentInstance();
            if (instance && instance->messages())
                instance->messages()->requestLoadChannel(currentChannelId);
        } else if (action == std::string("open-image")) {
            std::string url = attribute(actionEl, "data-url");
            if (!url.empty()) {
                std::vector<std::string> gallery;
                if (auto *msgEl = closest(actionEl, "data-message-id")) {
                    ::Rml::ElementList imgs;
                    msgEl->GetElementsByTagName(imgs, "img");
                    for (auto *img : imgs) {
                        std::string iUrl = attribute(img, "data-url");
                        if (!iUrl.empty() && std::find(gallery.begin(), gallery.end(), iUrl) == gallery.end())
                            gallery.push_back(iUrl);
                    }
                }
                shell.openImage(std::string(url), gallery);
            }
        } else if (action == std::string("open-attachment")) {
            std::string url = attribute(actionEl, "data-url");
            if (!url.empty())
                Platform::Desktop::openUrl(url);
        } else if (action == std::string("reveal-attachment-spoiler")) {
            uint64_t attId = parseNumber(attribute(actionEl, "data-attachment-id"));
            if (attId != 0 && shell.chat())
                shell.chat()->revealSpoiler(Core::Snowflake(attId));
        } else if (action == std::string("reveal-text-spoiler")) {
            actionEl->SetClass("blurred", false);
            requestRender();
        } else if (action == std::string("toggle-reaction")) {
            Core::ClientInstance *instance = shell.currentInstance();
            if (instance && instance->messages()) {
                uint64_t mid = parseNumber(attribute(actionEl, "data-message-id"));
                uint64_t eid = parseNumber(attribute(actionEl, "data-emoji-id"));
                std::string ename = attribute(actionEl, "data-emoji-name");
                bool animated = attribute(actionEl, "data-emoji-animated") == std::string("1");
                bool me = attribute(actionEl, "data-me") == std::string("1");
                bool isBurst = attribute(actionEl, "data-burst") == std::string("1");

                Discord::Emoji emoji = (eid != 0)
                                           ? Discord::Emoji::custom(Core::Snowflake(eid), ename, animated)
                                           : Discord::Emoji::unicode(ename);

                if (me)
                    instance->messages()->removeReaction(currentChannelId, Core::Snowflake(mid), emoji, isBurst, Discord::Client::ReactionLocation::InlineButton);
                else
                    instance->messages()->addReaction(currentChannelId, Core::Snowflake(mid), emoji, isBurst, Discord::Client::ReactionLocation::InlineButton);
            }
        } else if (action == std::string("toolbar-react")) {
            uint64_t mid = parseNumber(attribute(actionEl, "data-message-id"));
            if (mid != 0)
                openReactionPickerForMessage(Core::Snowflake(mid));
        } else if (action == std::string("toolbar-reply")) {
            uint64_t mid = parseNumber(attribute(actionEl, "data-message-id"));
            std::string author = attribute(actionEl, "data-author");
            setReplyTarget(Core::Snowflake(mid), author, std::string());
        } else if (action == std::string("toolbar-more")) {
            uint64_t mid = parseNumber(attribute(actionEl, "data-message-id"));
            if (mid != 0)
                showMessageContextMenu(Core::Snowflake(mid));
        } else if (action == std::string("composer-reply-close")) {
            clearReplyTarget();
            clearEditTarget();
        } else if (action == std::string("composer-attach")) {
            Platform::Guard<ChatController> self(this);
            Platform::Desktop::openFiles("Select Files", {{"All Files", "*.*"}}, [self](std::vector<std::string> files) {
                if (!self) return;
                for (const auto &file : files) {
                    auto path = Platform::utf8Path(file);
                    Core::PendingAttachment attachment;
                    attachment.filePath = file;
                    attachment.filename = Platform::pathUtf8(path.filename());
                    attachment.size = static_cast<int64_t>(std::filesystem::file_size(path));
                    self->pendingAttachments.push_back(std::move(attachment));
                }
                self->updateComposer();
            });
        } else if (action == std::string("composer-emoji")) {
            openEmojiPickerForComposer();
        } else if (action == std::string("composer-remove-attachment")) {
            int idx = parseNumber<int>(attribute(actionEl, "data-index"));
            if (idx >= 0 && idx < pendingAttachments.size()) {
                pendingAttachments.erase(pendingAttachments.begin() + idx);
                updateComposer();
            }
        } else if (action == std::string("channel-mention")) {
            uint64_t cid = parseNumber(attribute(actionEl, "data-channel-id"));
            if (cid != 0)
                shell.openChannel(currentAccountId, currentGuildId, Core::Snowflake(cid));
        } else if (action == std::string("channel-link")) {
            std::string url = attribute(actionEl, "data-href");
            if (auto link = Discord::ChannelLink::parse(url)) {
                if (link->messageId.isValid())
                    shell.jumpToMessage(link->channelId, link->messageId);
                else
                    shell.openChannel(currentAccountId, link->guildId, link->channelId);
            }
        } else if (action == std::string("link")) {
            std::string url = attribute(actionEl, "data-href");
            Rml::showConfirmModal(
                shell,
                std::string("External Link"),
                "Are you sure you want to open <b>" + escape(url) + "</b>?",
                std::string("Open Link"),
                [url]() { Platform::Desktop::openUrl(url); },
                nullptr,
                false);
        }
    });

    // Scroll listener on #messages
    if (messagesEl) {
        listen("scroll", [this](::Rml::Event &) {
            if (!messagesEl)
                return;

            float scrollTop = messagesEl->GetScrollTop();
            float clientHeight = messagesEl->GetClientHeight();
            float scrollHeight = messagesEl->GetScrollHeight();
            bool atBottom = (scrollTop + clientHeight >= scrollHeight - 30.0f);

            if (wasAtBottom != atBottom) {
                wasAtBottom = atBottom;
                Core::ClientInstance *instance = shell.currentInstance();
                if (instance && instance->readState())
                    instance->readState()->setActiveChannelAtBottom(atBottom);
            }

            // Load older messages when scrolling near top
            if (scrollTop <= 60.0f) {
                checkAndLoadOlder();
            }

            // Load newer messages when scrolling near bottom and not at latest
            if (atBottom && shell.chat() && !shell.chat()->isAtLatest()) {
                checkAndLoadNewer();
            }
        }, messagesEl);
    }

    // Keydown listener on #composer
    if (composerEl) {
        listen("change", [this](::Rml::Event &) {
            updateComposerPlaceholder();
        }, composerEl);
        listen("keydown", [this](::Rml::Event &event) {
            int key = event.GetParameter<int>("key_identifier", 0);
            const bool shift = event.GetParameter<int>("shift_key", 0) != 0;

            if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER) {
                if (shift) {
                    event.StopPropagation();
                    if (hasSendPermission && textareaEl)
                        textareaEl->GetContext()->ProcessTextInput('\n');
                    return;
                } else {
                    event.StopPropagation();
                    if (!hasSendPermission)
                        return;

                    std::string text;
                    if (textareaEl)
                        text = Platform::trim(textareaEl->GetValue());

                    if (text.empty() && pendingAttachments.empty())
                        return;

                    if (editingMessageId.isValid()) {
                        Core::ClientInstance *instance = shell.currentInstance();
                        if (instance && instance->discord()) {
                            instance->discord()->editMessage(currentChannelId, editingMessageId, text);
                        }
                        clearEditTarget();
                    } else {
                        shell.sendMessage(text, pendingAttachments, replyTargetId);
                        clearReplyTarget();
                        pendingAttachments.clear();
                        if (textareaEl)
                            textareaEl->SetValue("");
                        updateComposer();
                    }
                    return;
                }
            } else if (key == ::Rml::Input::KI_ESCAPE) {
                if (editingMessageId.isValid()) {
                    event.StopPropagation();
                    clearEditTarget();
                } else if (replyTargetId.isValid()) {
                    event.StopPropagation();
                    clearReplyTarget();
                } else if (!pendingAttachments.empty()) {
                    event.StopPropagation();
                    pendingAttachments.clear();
                    updateComposer();
                }
            } else if (key == ::Rml::Input::KI_UP) {
                std::string currentVal;
                if (textareaEl)
                    currentVal = std::string(textareaEl->GetValue());
                if (currentVal.empty() && hasSendPermission) {
                    ChatModel *model = shell.chat();
                    Core::ClientInstance *instance = shell.currentInstance();
                    if (model && instance) {
                        Core::Snowflake selfId = instance->accountId();
                        for (int r = model->rowCount() - 1; r >= 0; --r) {
                            const auto message = model->row(r);
                            if (message.userId == selfId && !message.isSystemMessage) {
                                event.StopPropagation();
                                setEditTarget(message.messageId, message.content);
                                break;
                            }
                        }
                    }
                }
            }
        }, composerEl, true);
    }
}

void ChatController::setupModelConnections()
{
    track(shell.locationChanged.connect([this](const Location &location) { onLocationChanged(location); }));
    track(shell.instanceChanged.connect([this](Core::ClientInstance *instance) { onInstanceChanged(instance); }));
    track(shell.memberListVisibleChanged.connect([this](bool) { updateHeader(); }));

    if (auto *model = shell.chat()) {
        track(model->rowsInserted.connect([this](int first, int last) { onRowsInserted(first, last); }));
        track(model->rowsRemoved.connect([this](int first, int last) { onRowsRemoved(first, last); }));
        track(model->rowsChanged.connect([this](int first, int last) { onDataChanged(first, last); }));
        track(model->modelReset.connect([this] { onModelReset(); }));
        track(model->atLatestChanged.connect([this](bool atLatest) { onAtLatestChanged(atLatest); }));
    }
    if (auto *typingTracker = shell.typing())
        track(typingTracker->typersChanged.connect([this] { onTypersChanged(); }));
}

void ChatController::onLocationChanged(const Location &location)
{
    currentChannelId = location.channelId;
    currentGuildId = location.guildId;
    currentAccountId = location.accountId;
    if (textareaEl)
        textareaEl->SetValue("");

    clearReplyTarget();
    clearEditTarget();
    pendingAttachments.clear();
    loadingHistory = false;
    wasAtBottom = true;

    updateHeader();
    updateTyping();
    updateComposer();
    renderAllMessages();
}

void ChatController::onInstanceChanged(Core::ClientInstance *instance)
{
    for (auto &connection : instanceConnections)
        connection.disconnect();
    instanceConnections.clear();
    if (instance) {
        instanceConnections.push_back(instance->presences()->presencesChanged.connect(
            [this](const std::vector<Core::Snowflake> &users) {
                if (std::find(users.begin(), users.end(), headerRecipientId) != users.end())
                    updateHeader();
            }));
        instanceConnections.push_back(instance->channelUpdated.connect(
            [this](const Discord::ChannelUpdate &channel) {
                if (channel.channel->id == currentChannelId) {
                    updateHeader();
                    updateComposer();
                }
            }));
    }
    currentAccountId = shell.location().accountId;
    updateHeader();
    updateTyping();
    updateComposer();
    renderAllMessages();
}

void ChatController::onRowsInserted(int first, int last)
{
    if (!messagesEl || !shell.document())
        return;

    ChatModel *model = shell.chat();
    if (!model)
        return;

    loadingHistory = false;

    if (first == 0) {
        // Prepending history: preserve scroll position
        float prevScrollTop = messagesEl->GetScrollTop();
        float prevScrollHeight = messagesEl->GetScrollHeight();

        ::Rml::Element *refChild = messagesEl->GetNumChildren() > 0 ? messagesEl->GetChild(0) : nullptr;
        for (int r = first; r <= last; ++r) {
            auto el = shell.document()->CreateElement("div");
            if (el) {
                el->SetInnerRML(buildMessageHtml(r));
                if (refChild)
                    messagesEl->InsertBefore(std::move(el), refChild);
                else
                    messagesEl->AppendChild(std::move(el));
            }
        }

        requestRender();
        float newScrollHeight = messagesEl->GetScrollHeight();
        messagesEl->SetScrollTop(prevScrollTop + (newScrollHeight - prevScrollHeight));
    } else {
        // Appending new messages
        for (int r = first; r <= last; ++r) {
            auto el = shell.document()->CreateElement("div");
            if (el) {
                el->SetInnerRML(buildMessageHtml(r));
                messagesEl->AppendChild(std::move(el));
            }
        }

        requestRender();
        if (wasAtBottom)
            messagesEl->SetScrollTop(messagesEl->GetScrollHeight());
    }

    updateJumpBar();
}

void ChatController::onRowsRemoved(int first, int last)
{
    if (!messagesEl)
        return;

    for (int r = last; r >= first; --r) {
        if (r >= 0 && r < messagesEl->GetNumChildren())
            messagesEl->RemoveChild(messagesEl->GetChild(r));
    }
    requestRender();
    updateJumpBar();
}

void ChatController::onDataChanged(int first, int last)
{
    if (!messagesEl)
        return;

    for (int r = first; r <= last; ++r) {
        if (r >= 0 && r < messagesEl->GetNumChildren()) {
            messagesEl->GetChild(r)->SetInnerRML(buildMessageHtml(r));
        }
    }
    requestRender();
}

void ChatController::onModelReset()
{
    loadingHistory = false;
    renderAllMessages();
}

void ChatController::onAtLatestChanged(bool)
{
    updateJumpBar();
}

void ChatController::onTypersChanged()
{
    updateTyping();
}

void ChatController::renderAllMessages()
{
    if (!messagesEl || !shell.document())
        return;

    ChatModel *model = shell.chat();
    if (!model) {
        messagesEl->SetInnerRML("");
        requestRender();
        return;
    }

    std::string allRml;
    int count = model->rowCount();
    for (int r = 0; r < count; ++r)
        allRml += "<div>" + buildMessageHtml(r) + "</div>";

    messagesEl->SetInnerRML(allRml);
    requestRender();

    if (wasAtBottom)
        messagesEl->SetScrollTop(messagesEl->GetScrollHeight());

    updateJumpBar();
}

void ChatController::checkAndLoadOlder()
{
    if (loadingHistory)
        return;

    ChatModel *model = shell.chat();
    Core::ClientInstance *instance = shell.currentInstance();
    if (!model || !instance || !instance->messages())
        return;

    Core::Snowflake oldestId = model->getOldestMessageId();
    if (oldestId.isValid()) {
        loadingHistory = true;
        instance->messages()->requestLoadHistory(currentChannelId, oldestId);
    }
}

void ChatController::checkAndLoadNewer()
{
    ChatModel *model = shell.chat();
    Core::ClientInstance *instance = shell.currentInstance();
    if (!model || !instance || !instance->messages())
        return;

    Core::Snowflake newestId = model->getNewestMessageId();
    if (newestId.isValid()) {
        instance->messages()->requestLoadFuture(currentChannelId, newestId);
    }
}

std::string ChatController::buildMessageHtml(int row)
{
    ChatModel *model = shell.chat();
    if (!model || row < 0 || row >= model->rowCount())
        return std::string();

    const auto message = model->row(row);
    const auto msgId = message.messageId;
    const auto authorId = message.userId;
    const auto &authorName = message.username;
    const auto authorColor = message.usernameColor;
    const auto timestamp = message.timestamp;
    const auto editedVal = message.editedTimestamp;
    const bool isPending = message.isPending, isError = message.isErrored, isSystem = message.isSystemMessage;
    const auto msgType = message.messageType;
    const auto &reply = message.replyData;
    const auto &attachments = message.attachments;
    const auto &embeds = message.embeds;
    const auto &reactions = message.reactions;
    const auto &stickers = message.stickers;
    const auto &rawContent = message.content;

    std::string out;

    // Date separator check
    bool showDateSep = message.dateSeparator;
    if (showDateSep) {
        out += std::format("<div class=\"date-separator\">"
            "<span class=\"date-separator-line\"></span>"
            "<span class=\"date-separator-text\">{0}</span>"
            "<span class=\"date-separator-line\"></span>"
            "</div>", formatDateSeparator(timestamp));
    }

    if (isSystem) {
        std::string icon = systemMessageIcon(msgType);
        std::string cls = systemMessageClass(msgType);
        std::string content = Platform::htmlEscape(rawContent);
        out += std::format("<div class=\"system-message\">"
            "<img class=\"system-icon {0}\" src=\"{1}\" />"
            "<span class=\"system-text\"><strong>{2}</strong> {3}</span>"
            "<span class=\"system-timestamp\">{4}</span>"
            "</div>", cls, icon, Platform::htmlEscape(authorName), content, formatGutterTimestamp(timestamp));
        return out;
    }

    const bool showHeader = message.showHeader;

    // Parse Markdown content and check for user mention
    RenderedMarkdown contentRendered = ChatMarkdown::toRml(rawContent, shell, currentAccountId, currentGuildId, currentChannelId);
    bool mentionsMe = contentRendered.mentionsUser;

    std::string rowClasses = std::string("message-row");
    if (showHeader)
        rowClasses += std::string(" group-start");
    if (mentionsMe)
        rowClasses += std::string(" mentions-me");
    if (isPending)
        rowClasses += std::string(" message-pending");
    if (isError)
        rowClasses += std::string(" message-error");

    out += std::format("<div class=\"{0}\" data-message-id=\"{1}\">", rowClasses, std::to_string(uint64_t(msgId)));

    // Reply preview
    if (reply.state != ReplyData::State::None) {
        Core::ClientInstance *instance = shell.currentInstance();
        std::string refAvatarUrl;
        if (instance && instance->users() && reply.authorId.isValid()) {
            auto refUser = instance->users()->getUser(reply.authorId);
            refAvatarUrl = refUser ? instance->users()->getAvatarUrl(*refUser, currentGuildId, 32) : Discord::Cdn::defaultUserAvatar(reply.authorId, 32);
        } else {
            refAvatarUrl = Discord::Cdn::defaultUserAvatar(reply.authorId, 32);
        }
        std::string refAvatarSrc = shell.imageSource(refAvatarUrl, Platform::Size(16, 16));
        std::string replyColorStyle = reply.authorColor.valid ? std::format("style=\"color:{0};\"", reply.authorColor.css()) : std::string();

        out += std::format("<div class=\"reply-bar-container\" data-action=\"jump-to-message\" data-message-id=\"{0}\">"
            "<div class=\"reply-spine\"></div>"
            "<div class=\"reply-avatar-clip\"><img class=\"reply-avatar\" src=\"{1}\" /></div>"
            "<span class=\"reply-author\" {2}>{3}</span>"
            "<span class=\"reply-content\">{4}</span>"
            "</div>", std::to_string(uint64_t(reply.referencedMessageId)), Platform::htmlEscape(refAvatarSrc), replyColorStyle, Platform::htmlEscape(reply.authorName), Platform::htmlEscape(reply.contentSnippet));
    }

    // Header / Group start
    if (showHeader) {
        Core::ClientInstance *instance = shell.currentInstance();
        std::string avatarUrl;
        if (instance && instance->users() && authorId.isValid()) {
            auto user = instance->users()->getUser(authorId);
            avatarUrl = user ? instance->users()->getAvatarUrl(*user, currentGuildId, 64) : Discord::Cdn::defaultUserAvatar(authorId, 64);
        } else {
            avatarUrl = Discord::Cdn::defaultUserAvatar(authorId, 64);
        }
        std::string avatarSrc = shell.imageSource(avatarUrl, Platform::Size(40, 40));
        std::string authorColorStyle = authorColor.valid ? std::format("style=\"color:{0};\"", authorColor.css()) : std::string();

        out += std::format("<div class=\"message-avatar-clip\" data-action=\"user-profile\" data-user-id=\"{1}\"><img class=\"message-avatar\" src=\"{0}\" /></div>"
            "<div class=\"message-header\">"
            "<span class=\"message-author\" {2} data-action=\"user-profile\" data-user-id=\"{1}\">{3}</span>"
            "<span class=\"message-timestamp\">{4}</span>"
            "</div>", Platform::htmlEscape(avatarSrc), std::to_string(uint64_t(authorId)), authorColorStyle, Platform::htmlEscape(authorName), formatTimestamp(timestamp));
    } else {
        out += std::format("<span class=\"gutter-timestamp\">{0}</span>", formatGutterTimestamp(timestamp));
    }

    // Body content
    out += std::string("<div class=\"message-body\">");
    out += contentRendered.rml;
    if (editedVal.valid()) {
        out += std::string("<span class=\"edited-marker\">(edited)</span>");
    }
    out += std::string("</div>");

    // Attachments
    for (const auto &att : attachments) {
        if (att.isMedia()) {
            if (att.isImage) {
                bool isSpoiler = att.isSpoiler && model && !model->isSpoilerRevealed(att.id);
                if (isSpoiler) {
                    out += std::format("<div class=\"attachment-spoiler-box\" data-action=\"reveal-attachment-spoiler\" data-attachment-id=\"{0}\">"
                        "<span class=\"spoiler-pill\">SPOILER</span>"
                        "</div>", uint64_t(att.id));
                } else {
                    Platform::Size disp = att.displaySize;
                    if (disp.empty())
                        disp = Platform::Size(400, 300);
                    if (disp.width > 400 || disp.height > 300)
                        disp = Platform::fitWithin(disp, {400, 300});

                    std::string src = shell.imageSource(!att.proxyUrl.empty() ? att.proxyUrl : att.originalUrl, disp);
                    out += std::format("<div class=\"attachment-container\">"
                        "<img class=\"attachment-image\" src=\"{0}\" data-action=\"open-image\" data-url=\"{1}\" style=\"width:{2}dp; height:{3}dp;\" />"
                        "</div>", Platform::htmlEscape(src), Platform::htmlEscape(att.originalUrl), std::to_string(disp.width), std::to_string(disp.height));
                }
            } else if (att.isVideo) {
                out += std::format("<div class=\"attachment-file-card\" data-action=\"open-attachment\" data-url=\"{0}\">"
                    "<img class=\"attachment-file-icon\" src=\"icon:image-play\" />"
                    "<div class=\"attachment-file-info\">"
                    "<span class=\"attachment-filename\">{1}</span>"
                    "<span class=\"attachment-filesize\">{2}</span>"
                    "</div>"
                    "</div>", Platform::htmlEscape(att.originalUrl), Platform::htmlEscape(att.filename), formatFileSize(att.fileSizeBytes));
            }
        } else {
            std::string iconName = att.isAudio ? std::string("icon:file-audio") : std::string("icon:file-text");
            out += std::format("<div class=\"attachment-file-card\" data-action=\"open-attachment\" data-url=\"{0}\">"
                "<img class=\"attachment-file-icon\" src=\"{1}\" />"
                "<div class=\"attachment-file-info\">"
                "<span class=\"attachment-filename\">{2}</span>"
                "<span class=\"attachment-filesize\">{3}</span>"
                "</div>"
                "</div>", Platform::htmlEscape(att.originalUrl), iconName, Platform::htmlEscape(att.filename), formatFileSize(att.fileSizeBytes));
        }
    }

    // Embeds
    for (const auto &embed : embeds) {
        std::string borderStyle = std::format("border-left: 4dp {0};", embed.color.valid ? embed.color.css() : std::string("#202225"));
        out += std::format("<div class=\"embed-card\" style=\"{0}\">", borderStyle);

        if (!embed.authorName.empty()) {
            out += std::string("<div class=\"embed-author\">");
            if (!embed.authorIconUrl.empty()) {
                std::string iconSrc = shell.imageSource(embed.authorIconUrl, Platform::Size(24, 24));
                out += std::format("<img class=\"embed-author-icon\" src=\"{0}\" />", Platform::htmlEscape(iconSrc));
            }
            if (!embed.authorUrl.empty()) {
                out += std::format("<a href=\"{0}\" class=\"chat-link\" data-action=\"link\" data-href=\"{0}\">{1}</a>", Platform::htmlEscape(embed.authorUrl), Platform::htmlEscape(embed.authorName));
            } else {
                out += std::format("<span>{0}</span>", Platform::htmlEscape(embed.authorName));
            }
            out += std::string("</div>");
        }

        if (!embed.title.empty()) {
            if (!embed.url.empty()) {
                out += std::format("<a href=\"{0}\" class=\"embed-title\" data-action=\"link\" data-href=\"{0}\">{1}</a>", Platform::htmlEscape(embed.url), Platform::htmlEscape(embed.title));
            } else {
                out += std::format("<div class=\"embed-title\">{0}</div>", Platform::htmlEscape(embed.title));
            }
        }

        if (!embed.description.empty()) {
            RenderedMarkdown desc = ChatMarkdown::toRml(embed.description, shell, currentAccountId, currentGuildId, currentChannelId);
            out += std::string("<div class=\"embed-description\">");
            out += desc.rml;
            out += std::string("</div>");
        }

        if (!embed.fields.empty()) {
            out += std::string("<div class=\"embed-fields\">");
            for (const auto &field : embed.fields) {
                out += std::format("<div class=\"embed-field{0}\">", field.isInline ? std::string() : std::string(" non-inline"));
                out += std::format("<div class=\"embed-field-name\">{0}</div>", Platform::htmlEscape(field.name));
                RenderedMarkdown fval = ChatMarkdown::toRml(field.value, shell, currentAccountId, currentGuildId, currentChannelId);
                out += std::string("<div class=\"embed-field-value\">");
                out += fval.rml;
                out += std::string("</div></div>");
            }
            out += std::string("</div>");
        }

        if (!embed.images.empty() && !embed.images.front().url.empty()) {
            const auto &eimg = embed.images.front();
            Platform::Size disp = eimg.displaySize;
            if (disp.empty() || disp.width > 400 || disp.height > 300)
                disp = !disp.empty() ? Platform::fitWithin(disp, {400, 300}) : Platform::Size(400, 300);
            std::string imgSrc = shell.imageSource(eimg.url, disp);
            out += std::format("<img class=\"embed-image\" src=\"{0}\" data-action=\"open-image\" data-url=\"{1}\" style=\"width:{2}dp; height:{3}dp;\" />", Platform::htmlEscape(imgSrc), Platform::htmlEscape(eimg.url), std::to_string(disp.width), std::to_string(disp.height));
        } else if (!embed.thumbnailUrl.empty()) {
            Platform::Size disp = embed.thumbnailSize;
            if (disp.empty() || disp.width > 80 || disp.height > 80)
                disp = Platform::Size(80, 80);
            std::string thumbSrc = shell.imageSource(embed.thumbnailUrl, disp);
            out += std::format("<img class=\"embed-image\" src=\"{0}\" data-action=\"open-image\" data-url=\"{1}\" style=\"width:{2}dp; height:{3}dp;\" />", Platform::htmlEscape(thumbSrc), Platform::htmlEscape(embed.thumbnailUrl), std::to_string(disp.width), std::to_string(disp.height));
        }

        if (!embed.footerText.empty()) {
            out += std::string("<div class=\"embed-footer\">");
            if (!embed.footerIconUrl.empty()) {
                std::string ficonSrc = shell.imageSource(embed.footerIconUrl, Platform::Size(20, 20));
                out += std::format("<img class=\"embed-footer-icon\" src=\"{0}\" />", Platform::htmlEscape(ficonSrc));
            }
            out += std::format("<span>{0}</span>", Platform::htmlEscape(embed.footerText));
            out += std::string("</div>");
        }

        out += std::string("</div>");
    }

    // Stickers
    for (const auto &sticker : stickers) {
        if (!sticker.stillUrl.empty() || !sticker.animatedUrl.empty()) {
            std::string sUrl = !sticker.stillUrl.empty() ? sticker.stillUrl : sticker.animatedUrl;
            std::string sSrc = shell.imageSource(sUrl, Platform::Size(160, 160));
            out += std::format("<img class=\"sticker-image\" src=\"{0}\" title=\"{1}\" alt=\"{1}\" />", Platform::htmlEscape(sSrc), Platform::htmlEscape(sticker.name));
        }
    }

    // Reactions
    if (!reactions.empty()) {
        out += std::string("<div class=\"reactions-container\">");
        for (const auto &rx : reactions) {
            std::string emojiContent;
            if (rx.emojiId.isValid()) {
                std::string eSrc = shell.imageSource(rx.emojiUrl, Platform::Size(16, 16));
                emojiContent = std::format("<img src=\"{0}\" alt=\":{1}:\" />", Platform::htmlEscape(eSrc), Platform::htmlEscape(rx.emojiName));
            } else {
                emojiContent = richText(rx.emojiName);
            }
            out += std::format("<div class=\"reaction-pill{0}\" data-action=\"toggle-reaction\" data-message-id=\"{1}\" data-emoji-name=\"{2}\" data-emoji-id=\"{3}\" data-emoji-animated=\"{4}\" data-me=\"{5}\" data-burst=\"{6}\">"
                "{7}"
                "<span class=\"reaction-count\">{8}</span>"
                "</div>", rx.me ? std::string(" me") : std::string(), std::to_string(uint64_t(msgId)), Platform::htmlEscape(rx.emojiName), rx.emojiId.isValid() ? std::to_string(uint64_t(rx.emojiId)) : std::string(), rx.emojiAnimated ? std::string("1") : std::string("0"), rx.me ? std::string("1") : std::string("0"), rx.isBurst ? std::string("1") : std::string("0"), emojiContent, std::to_string(rx.count));
        }
        out += std::string("</div>");
    }

    // Hover toolbar
    out += std::format("<div class=\"message-toolbar\">"
        "<div class=\"toolbar-button\" data-action=\"toolbar-react\" data-message-id=\"{0}\" title=\"Add Reaction\"><img src=\"icon:smile-plus\" /></div>"
        "<div class=\"toolbar-button\" data-action=\"toolbar-reply\" data-message-id=\"{0}\" data-author=\"{1}\" title=\"Reply\"><img src=\"icon:reply\" /></div>"
        "<div class=\"toolbar-button\" data-action=\"toolbar-more\" data-message-id=\"{0}\" title=\"More\"><img src=\"icon:ellipsis\" /></div>"
        "</div>", std::to_string(uint64_t(msgId)), Platform::htmlEscape(authorName));

    out += std::string("</div>");
    return out;
}

void ChatController::openSearch()
{
    std::string scope = conversationName();
    if (!shell.location().isHome()) {
        auto *instance = shell.currentInstance();
        auto guild = instance ? instance->getGuild(currentGuildId) : std::nullopt;
        scope = guild ? guild->name.valueOr(std::string()) : std::string();
    }
    shell.openSearch(currentChannelId, scope);
}

std::string ChatController::conversationName() const
{
    if (auto *tree = shell.channelTree()) {
        if (const auto *node = tree->findChannelTreeNode(currentChannelId, currentAccountId))
            return node->name;
    }
    if (auto *instance = shell.currentInstance()) {
        if (const auto channel = instance->getChannel(currentChannelId)) {
            if (channel->name.hasValue() && !channel->name->empty())
                return *channel->name;
        }
    }
    return {};
}

void ChatController::updateHeader()
{
    headerRecipientId = Core::Snowflake::Invalid;
    if (!headerEl)
        return;
    if (!currentChannelId.isValid()) {
        headerEl->SetInnerRML("");
        return;
    }

    auto *instance = shell.currentInstance();
    const bool isDm = shell.location().isHome();
    const std::string name = conversationName();
    std::string leadingIcon;
    std::string presenceDot;
    std::string topicHtml;

    if (isDm) {
        Core::Snowflake recipientId;
        std::string avatarHash;
        if (auto *tree = shell.channelTree()) {
            if (const auto *node = tree->findChannelTreeNode(currentChannelId, currentAccountId)) {
                recipientId = node->dmRecipientId;
                avatarHash = node->dmAvatarHash;
            }
        }
        if (recipientId.isValid() && instance) {
            headerRecipientId = recipientId;
            const std::string avatar = avatarHash.empty()
                ? Discord::Cdn::defaultUserAvatar(recipientId)
                : Discord::Cdn::userAvatar(recipientId, avatarHash, 32);
            leadingIcon = std::format("<div class=\"header-avatar-clip\"><img src=\"{0}\"/></div>", shell.imageSource(avatar, Platform::Size(24, 24)));
            const auto badge = instance->presences()->badge(recipientId);
            std::string status = std::string("status-offline");
            if (badge.streaming)
                status = std::string("status-streaming");
            else if (badge.status == Discord::StatusType::ONLINE)
                status = std::string("status-online");
            else if (badge.status == Discord::StatusType::IDLE)
                status = std::string("status-idle");
            else if (badge.status == Discord::StatusType::DND)
                status = std::string("status-dnd");
            presenceDot = std::format("<span class=\"header-presence {0}\"></span>", status);
        } else {
            leadingIcon = std::string("<img class=\"header-icon\" src=\"icon:at-sign\"/>");
        }
    } else {
        std::string icon = std::string("icon:hash");
        if (instance) {
            if (const auto channel = instance->getChannel(currentChannelId)) {
                if (channel->isVoice())
                    icon = std::string("icon:volume-2");
                else if (channel->type.valueOr(Discord::ChannelType::GUILD_TEXT) == Discord::ChannelType::GUILD_NEWS)
                    icon = std::string("icon:megaphone");
                if (channel->topic.hasValue() && !channel->topic->empty()) {
                    const auto topic = ChatMarkdown::toRml(Platform::simplified(*channel->topic), shell,
                        currentAccountId, currentGuildId, currentChannelId);
                    topicHtml = std::string("<span class=\"header-divider\"></span><div class=\"header-topic\">")
                        + topic.rml + std::string("</div>");
                }
            }
        }
        leadingIcon = std::format("<img class=\"header-icon\" src=\"{0}\"/>", icon);
    }

    const std::string threads = isDm ? std::string() : std::string("<div class=\"header-btn\" data-action=\"open-threads\" title=\"Threads\"><img src=\"icon:messages-square\"/></div>");
    const std::string members = std::format("<div class=\"header-btn{0}\" data-action=\"toggle-members\" title=\"{1}\"><img src=\"icon:id-card\"/></div>", shell.isMemberListVisible() ? std::string(" active") : std::string(), isDm ? std::string("Conversation profile") : std::string("Member list"));
    const std::string html = std::string("<div class=\"header-left\">") + leadingIcon
        + std::string("<span class=\"header-title\">") + escape(name) + std::string("</span>")
        + presenceDot + topicHtml + std::string("</div><div class=\"header-right\">")
        + threads + members + std::string("<div class=\"header-btn\" data-action=\"open-search\" title=\"Search\"><img src=\"icon:search\"/></div></div>");
    headerEl->SetInnerRML(html);
    requestRender();
}

void ChatController::updateTyping()
{
    if (!typingEl)
        return;

    auto typers = shell.typing() ? shell.typing()->getActiveTypers() : std::vector<Core::TyperInfo>();
    if (typers.empty()) {
        typingEl->SetInnerRML("");
        requestRender();
        return;
    }

    std::string text;
    if (typers.size() == 1) {
        text = std::format("<b>{0}</b> is typing…", Platform::htmlEscape(typers[0].name));
    } else if (typers.size() == 2) {
        text = std::format("<b>{0}</b> and <b>{1}</b> are typing…", Platform::htmlEscape(typers[0].name), Platform::htmlEscape(typers[1].name));
    } else if (typers.size() == 3) {
        text = std::format("<b>{0}</b>, <b>{1}</b>, and <b>{2}</b> are typing…", Platform::htmlEscape(typers[0].name), Platform::htmlEscape(typers[1].name), Platform::htmlEscape(typers[2].name));
    } else {
        text = std::string("Several people are typing…");
    }

    std::string html = std::format("<div class=\"typing-dots\">"
        "<span class=\"typing-dot\"></span>"
        "<span class=\"typing-dot\"></span>"
        "<span class=\"typing-dot\"></span>"
        "</div>"
        "<span class=\"typing-text\">{0}</span>", text);

    typingEl->SetInnerRML(html);
    requestRender();
}

void ChatController::updateComposer()
{
    if (!composerEl)
        return;
    if (!currentChannelId.isValid()) {
        textareaEl = nullptr;
        composerEl->SetInnerRML("");
        return;
    }
    const ::Rml::String draft = textareaEl ? textareaEl->GetValue() : ::Rml::String();
    const bool restoreFocus = textareaEl && textareaEl->IsPseudoClassSet("focus");

    Core::ClientInstance *instance = shell.currentInstance();
    hasSendPermission = instance != nullptr;
    channelRateLimit = 0;
    std::string placeholder = std::string("Message...");

    if (instance) {
        auto ch = instance->getChannel(currentChannelId);
        bool isDm = shell.location().isHome();
        const std::string chName = conversationName();

        if (isDm) {
            placeholder = std::string("Message @") + chName;
            hasSendPermission = true;
        } else {
            placeholder = std::string("Message #") + chName;
            Core::Snowflake permChannel = currentChannelId;
            Discord::Permission sendPerm = Discord::Permission::SEND_MESSAGES;
            if (ch && ch->isThread()) {
                if (ch->parentId.hasValue())
                    permChannel = *ch->parentId;
                sendPerm = Discord::Permission::SEND_MESSAGES_IN_THREADS;
            }

            hasSendPermission = instance->permissions()->hasChannelPermission(instance->accountId(), permChannel, sendPerm);
            if (ch && ch->isArchived())
                hasSendPermission = false;

            channelRateLimit = instance->getChannelRateLimit(currentChannelId);
            bool canBypass = instance->permissions()->hasChannelPermission(instance->accountId(), permChannel, Discord::Permission::BYPASS_SLOWMODE);
            if (canBypass)
                channelRateLimit = 0;
        }
    }

    if (!hasSendPermission)
        placeholder = std::string("You do not have permission to send messages in this channel.");

    // Reply bar HTML
    std::string replyBarHtml;
    if (replyTargetId.isValid()) {
        replyBarHtml = std::format("<div class=\"composer-reply-bar\">"
            "<div class=\"composer-reply-text\">Replying to <strong>{0}</strong> <span>{1}</span></div>"
            "<img class=\"composer-reply-close\" src=\"icon:x\" data-action=\"composer-reply-close\" title=\"Cancel reply\" />"
            "</div>", Platform::htmlEscape(replyAuthorName), Platform::htmlEscape(replySnippet));
    } else if (editingMessageId.isValid()) {
        replyBarHtml = std::string("<div class=\"composer-reply-bar\">"
            "<div class=\"composer-reply-text\">Editing message — <span>Escape to cancel</span></div>"
            "<img class=\"composer-reply-close\" src=\"icon:x\" data-action=\"composer-reply-close\" title=\"Cancel edit\" />"
            "</div>");
    }

    // Attachments bar HTML
    std::string attachmentsHtml;
    if (!pendingAttachments.empty()) {
        attachmentsHtml = std::string("<div class=\"composer-attachments-bar\">");
        for (int i = 0; i < pendingAttachments.size(); ++i) {
            const auto &att = pendingAttachments[i];
            attachmentsHtml += std::format("<div class=\"composer-attachment-item\">"
                "<span>{0} ({1})</span>"
                "<img class=\"composer-attachment-remove\" src=\"icon:x\" data-action=\"composer-remove-attachment\" data-index=\"{2}\" title=\"Remove\" />"
                "</div>", Platform::htmlEscape(att.filename), formatFileSize(att.size), std::to_string(i));
        }
        attachmentsHtml += std::string("</div>");
    }

    // Slowmode notice HTML
    std::string slowmodeHtml;
    if (channelRateLimit > 0) {
        slowmodeHtml = std::format("<div class=\"slowmode-notice\"><img src=\"icon:clock\" style=\"width:14dp;height:14dp;image-color:#f0b232;\" /> Slowmode is active ({0}s)</div>", channelRateLimit);
    }

    std::string composerHtml = std::format("{0}"
        "{1}"
        "<div class=\"composer-main-row\">"
        "<div class=\"composer-btn\" data-action=\"composer-attach\" title=\"Attach file\"><img src=\"icon:image\" /></div>"
        "<div class=\"composer-input-wrap\">"
        "<span id=\"composer-placeholder\" class=\"composer-placeholder\">{3}</span>"
        "<textarea id=\"composer-input\" class=\"composer-textarea{2}\" rows=\"1\"{4}></textarea>"
        "</div>"
        "<div class=\"composer-btn\" data-action=\"composer-emoji\" title=\"Select emoji\"><img src=\"icon:smile\" /></div>"
        "</div>"
        "{5}", replyBarHtml, attachmentsHtml, hasSendPermission ? std::string() : std::string(" composer-disabled"), Platform::htmlEscape(placeholder), hasSendPermission ? std::string() : std::string(" disabled"), slowmodeHtml);

    textareaEl = nullptr;
    composerEl->SetInnerRML(composerHtml);
    textareaEl = dynamic_cast<::Rml::ElementFormControlTextArea*>(composerEl->GetElementById("composer-input"));
    if (textareaEl) {
        textareaEl->SetValue(draft);
        if (restoreFocus)
            textareaEl->Focus();
    }
    updateComposerPlaceholder();
    requestRender();
}

void ChatController::updateComposerPlaceholder()
{
    if (!composerEl)
        return;
    if (auto *placeholder = composerEl->GetElementById("composer-placeholder"))
        placeholder->SetProperty("display", textareaEl && !textareaEl->GetValue().empty() ? "none" : "block");
    requestRender();
}

void ChatController::updateJumpBar()
{
    ChatModel *model = shell.chat();
    if (!model || !messagesEl)
        return;

    bool showJump = !model->isAtLatest();
    ::Rml::Element *jumpBar = messagesEl->GetElementById("jump-present-bar");

    if (showJump && !jumpBar && shell.document()) {
        auto barEl = shell.document()->CreateElement("div");
        if (barEl) {
            barEl->SetId("jump-present-bar");
            barEl->SetClass("jump-to-present-bar", true);
            barEl->SetInnerRML("<span class=\"jump-text\">You're viewing older messages.</span><span class=\"jump-button\" data-action=\"jump-to-present\">Jump to present</span>");
            messagesEl->AppendChild(std::move(barEl));
            requestRender();
        }
    } else if (!showJump && jumpBar) {
        messagesEl->RemoveChild(jumpBar);
        requestRender();
    }
}

void ChatController::setReplyTarget(Core::Snowflake messageId, const std::string &authorName, const std::string &snippet)
{
    replyTargetId = messageId;
    replyAuthorName = authorName;
    replySnippet = snippet;
    editingMessageId = Core::Snowflake::Invalid;
    updateComposer();
    if (textareaEl)
        textareaEl->Focus();
}

void ChatController::clearReplyTarget()
{
    replyTargetId = Core::Snowflake::Invalid;
    replyAuthorName.clear();
    replySnippet.clear();
    updateComposer();
}

void ChatController::setEditTarget(Core::Snowflake messageId, const std::string &content)
{
    editingMessageId = messageId;
    replyTargetId = Core::Snowflake::Invalid;
    updateComposer();
    if (textareaEl) {
        textareaEl->SetValue(content);
        updateComposerPlaceholder();
        textareaEl->Focus();
    }
}

void ChatController::clearEditTarget()
{
    editingMessageId = Core::Snowflake::Invalid;
    if (textareaEl)
        textareaEl->SetValue("");
    updateComposer();
}

void ChatController::showMessageContextMenu(Core::Snowflake messageId)
{
    ChatModel *model = shell.chat();
    if (!model)
        return;
    const int row = model->rowForMessage(messageId);
    if (row < 0)
        return;

    const auto message = model->row(row);
    const auto authorId = message.userId;
    const auto content = message.content;
    Core::ClientInstance *instance = shell.currentInstance();
    bool isSelf = (instance && authorId == instance->accountId());

    const auto accountId = currentAccountId;
    const auto channelId = currentChannelId;
    const auto guildId = currentGuildId;
    Platform::Guard<Core::ClientInstance> instanceGuard(instance);
    const auto author = message.username;
    std::vector<MenuItem> menu;
    menu.push_back({ std::string("Add Reaction"), [this, accountId, channelId, messageId] {
        shell.openExpressionPickerForReaction(accountId, channelId, messageId, pickerAnchor());
    } });
    menu.push_back({ std::string("Reply"), [this, messageId, author, content, accountId, channelId] {
        if (currentAccountId == accountId && currentChannelId == channelId)
            setReplyTarget(messageId, author, Platform::truncateUtf8(content, 50));
    } });
    if (isSelf) {
        menu.push_back({ std::string("Edit Message"), [this, messageId, content, accountId, channelId] {
            if (currentAccountId == accountId && currentChannelId == channelId)
                setEditTarget(messageId, content);
        } });
        menu.push_back({ std::string("Delete Message"), [this, messageId, instanceGuard, channelId] {
            if (instanceGuard) {
                showConfirmModal(shell, std::string("Delete Message"),
                    std::string("Are you sure you want to delete this message?"), std::string("Delete"),
                    [instanceGuard, channelId, messageId] {
                        if (instanceGuard)
                            instanceGuard->discord()->deleteMessage(channelId, messageId);
                    }, nullptr, true);
            }
        }, true, true });
    }
    menu.push_back({ {}, {}, false, false, true });
    menu.push_back({ "Copy Text", [content] { Platform::Desktop::setClipboardText(content); } });
    menu.push_back({ std::string("Copy Message Link"), [guildId, channelId, messageId] {
        const std::string guildPart = guildId.isValid() ? std::to_string(uint64_t(guildId)) : std::string("@me");
        Platform::Desktop::setClipboardText(std::format("https://discord.com/channels/{}/{}/{}", guildPart, channelId.toString(), messageId.toString()));
    } });
    menu.push_back({ std::string("Copy ID"), [messageId] {
        Platform::Desktop::setClipboardText(messageId.toString());
    } });
    shell.showContextMenu(std::move(menu));
}

void ChatController::openReactionPickerForMessage(Core::Snowflake messageId)
{
    shell.openExpressionPickerForReaction(currentAccountId, currentChannelId, messageId, pickerAnchor());
}

void ChatController::openEmojiPickerForComposer()
{
    if (auto *picker = shell.expressionPicker()) {
        emojiConnection.disconnect();
        gifConnection.disconnect();
        emojiConnection = picker->emojiPicked.connect([this](const Core::PickerEmoji &emoji, bool) {
            if (textareaEl) {
                std::string current = std::string(textareaEl->GetValue());
                std::string toInsert = emoji.messageText();
                textareaEl->SetValue((current + toInsert));
                updateComposerPlaceholder();
                textareaEl->Focus();
            }
        });

        gifConnection = picker->gifPicked.connect([this](const std::string &url) {
            shell.sendMessage(url);
        });
    }

    shell.openExpressionPickerForChat(currentAccountId, currentChannelId, pickerAnchor());
}

} // namespace Rml
} // namespace UI
} // namespace Styx