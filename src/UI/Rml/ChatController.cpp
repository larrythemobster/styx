#include "ChatController.hpp"
#include "ChatMarkdown.hpp"
#include "RmlRuntime.hpp"
#include "Shell.hpp"

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

#include <QClipboard>
#include <QCursor>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>

#include <utility>
#include <QFileInfo>
#include <QGuiApplication>

namespace Styx {
namespace UI {
namespace Rml {

namespace {

QString formatTimestamp(const QDateTime &dt)
{
    QDateTime local = dt.toLocalTime();
    QDate date = local.date();
    QDate today = QDate::currentDate();
    QString timeStr = local.toString(QStringLiteral("h:mm AP"));

    if (date == today) {
        return QStringLiteral("Today at ") + timeStr;
    } else if (date == today.addDays(-1)) {
        return QStringLiteral("Yesterday at ") + timeStr;
    } else {
        return date.toString(QStringLiteral("MM/dd/yyyy")) + QStringLiteral(" ") + timeStr;
    }
}

QString formatGutterTimestamp(const QDateTime &dt)
{
    return dt.toLocalTime().toString(QStringLiteral("h:mm AP"));
}

QString formatDateSeparator(const QDateTime &dt)
{
    return dt.toLocalTime().date().toString(QStringLiteral("MMMM d, yyyy"));
}

QString formatFileSize(qint64 bytes)
{
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 2) + QStringLiteral(" MB");
}

QString systemMessageIcon(Discord::MessageType type)
{
    switch (type) {
    case Discord::MessageType::USER_JOIN:
    case Discord::MessageType::RECIPIENT_ADD:
        return QStringLiteral("icon:arrow-right");
    case Discord::MessageType::RECIPIENT_REMOVE:
        return QStringLiteral("icon:arrow-left");
    case Discord::MessageType::CALL:
        return QStringLiteral("icon:phone");
    case Discord::MessageType::CHANNEL_PINNED_MESSAGE:
        return QStringLiteral("icon:pin");
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_1:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_2:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_3:
        return QStringLiteral("icon:gem");
    case Discord::MessageType::THREAD_CREATED:
    case Discord::MessageType::THREAD_STARTER_MESSAGE:
        return QStringLiteral("icon:messages-square");
    default:
        return QStringLiteral("icon:message-circle");
    }
}

QString systemMessageClass(Discord::MessageType type)
{
    switch (type) {
    case Discord::MessageType::USER_JOIN:
    case Discord::MessageType::RECIPIENT_ADD:
        return QStringLiteral("join");
    case Discord::MessageType::RECIPIENT_REMOVE:
        return QStringLiteral("leave");
    case Discord::MessageType::CALL:
        return QStringLiteral("call");
    case Discord::MessageType::CHANNEL_PINNED_MESSAGE:
        return QStringLiteral("pin");
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_1:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_2:
    case Discord::MessageType::PREMIUM_GUILD_SUBSCRIPTION_TIER_3:
        return QStringLiteral("boost");
    case Discord::MessageType::THREAD_CREATED:
    case Discord::MessageType::THREAD_STARTER_MESSAGE:
        return QStringLiteral("thread");
    default:
        return QStringLiteral("generic");
    }
}

} // namespace

ChatController::ChatController(Shell &shell, ::Rml::Element *root, QObject *parent)
    : Controller(shell, root, parent)
{
    initElements();
    setupEventListeners();
    setupModelConnections();

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
                QString url = attribute(linkEl, "href");
                if (url.startsWith(QStringLiteral("styx://channel/"))) {
                    quint64 chId = url.mid(18).toULongLong();
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
                    tr("External Link"),
                    QString(tr("Are you sure you want to open <b>%1</b>?")).arg(url.toHtmlEscaped()),
                    tr("Open Link"),
                    [url]() { QDesktopServices::openUrl(QUrl(url)); },
                    nullptr,
                    false);
                return;
            }
            return;
        }

        QString action = attribute(actionEl, "data-action");
        if (action == QStringLiteral("toggle-members")) {
            shell.setMemberListVisible(!shell.isMemberListVisible());
            updateHeader();
        } else if (action == QStringLiteral("open-threads")) {
            shell.openThreadBrowser(currentChannelId);
        } else if (action == QStringLiteral("user-profile")) {
            quint64 uid = attribute(actionEl, "data-user-id").toULongLong();
            if (uid != 0) {
                QRectF anchorRect(actionEl->GetAbsoluteLeft(), actionEl->GetAbsoluteTop(), actionEl->GetClientWidth(), actionEl->GetClientHeight());
                shell.showUserProfile(Core::Snowflake(uid), currentGuildId, anchorRect);
            }
        } else if (action == QStringLiteral("jump-to-message")) {
            quint64 mid = attribute(actionEl, "data-message-id").toULongLong();
            if (mid != 0)
                shell.jumpToMessage(currentChannelId, Core::Snowflake(mid));
        } else if (action == QStringLiteral("jump-to-present")) {
            Core::ClientInstance *instance = shell.currentInstance();
            if (instance && instance->messages())
                instance->messages()->requestLoadChannel(currentChannelId);
        } else if (action == QStringLiteral("open-image")) {
            QString url = attribute(actionEl, "data-url");
            if (!url.isEmpty()) {
                QList<QUrl> gallery;
                if (auto *msgEl = closest(actionEl, "data-message-id")) {
                    ::Rml::ElementList imgs;
                    msgEl->GetElementsByTagName(imgs, "img");
                    for (auto *img : imgs) {
                        QString iUrl = attribute(img, "data-url");
                        if (!iUrl.isEmpty() && !gallery.contains(QUrl(iUrl)))
                            gallery.append(QUrl(iUrl));
                    }
                }
                shell.openImage(QUrl(url), gallery);
            }
        } else if (action == QStringLiteral("open-attachment")) {
            QString url = attribute(actionEl, "data-url");
            if (!url.isEmpty())
                QDesktopServices::openUrl(QUrl(url));
        } else if (action == QStringLiteral("reveal-attachment-spoiler")) {
            quint64 attId = attribute(actionEl, "data-attachment-id").toULongLong();
            if (attId != 0 && shell.chat())
                shell.chat()->revealSpoiler(Core::Snowflake(attId));
        } else if (action == QStringLiteral("reveal-text-spoiler")) {
            actionEl->SetClass("blurred", false);
            requestRender();
        } else if (action == QStringLiteral("toggle-reaction")) {
            Core::ClientInstance *instance = shell.currentInstance();
            if (instance && instance->messages()) {
                quint64 mid = attribute(actionEl, "data-message-id").toULongLong();
                quint64 eid = attribute(actionEl, "data-emoji-id").toULongLong();
                QString ename = attribute(actionEl, "data-emoji-name");
                bool animated = attribute(actionEl, "data-emoji-animated") == QStringLiteral("1");
                bool me = attribute(actionEl, "data-me") == QStringLiteral("1");
                bool isBurst = attribute(actionEl, "data-burst") == QStringLiteral("1");

                Discord::Emoji emoji = (eid != 0)
                                           ? Discord::Emoji::custom(Core::Snowflake(eid), ename, animated)
                                           : Discord::Emoji::unicode(ename);

                if (me)
                    instance->messages()->removeReaction(currentChannelId, Core::Snowflake(mid), emoji, isBurst, Discord::Client::ReactionLocation::InlineButton);
                else
                    instance->messages()->addReaction(currentChannelId, Core::Snowflake(mid), emoji, isBurst, Discord::Client::ReactionLocation::InlineButton);
            }
        } else if (action == QStringLiteral("toolbar-react")) {
            quint64 mid = attribute(actionEl, "data-message-id").toULongLong();
            if (mid != 0)
                openReactionPickerForMessage(Core::Snowflake(mid));
        } else if (action == QStringLiteral("toolbar-reply")) {
            quint64 mid = attribute(actionEl, "data-message-id").toULongLong();
            QString author = attribute(actionEl, "data-author");
            setReplyTarget(Core::Snowflake(mid), author, QString());
        } else if (action == QStringLiteral("toolbar-more")) {
            quint64 mid = attribute(actionEl, "data-message-id").toULongLong();
            if (mid != 0)
                showMessageContextMenu(Core::Snowflake(mid));
        } else if (action == QStringLiteral("composer-reply-close")) {
            clearReplyTarget();
            clearEditTarget();
        } else if (action == QStringLiteral("composer-attach")) {
            QStringList files = QFileDialog::getOpenFileNames(
                &shell,
                tr("Select Files"),
                QString(),
                tr("All Files (*.*)"));
            if (!files.isEmpty()) {
                for (const auto &filePath : files) {
                    QFileInfo fi(filePath);
                    Core::PendingAttachment att;
                    att.filePath = filePath;
                    att.filename = fi.fileName();
                    att.size = fi.size();
                    pendingAttachments.append(att);
                }
                updateComposer();
            }
        } else if (action == QStringLiteral("composer-emoji")) {
            openEmojiPickerForComposer();
        } else if (action == QStringLiteral("composer-remove-attachment")) {
            int idx = attribute(actionEl, "data-index").toInt();
            if (idx >= 0 && idx < pendingAttachments.size()) {
                pendingAttachments.removeAt(idx);
                updateComposer();
            }
        } else if (action == QStringLiteral("channel-mention")) {
            quint64 cid = attribute(actionEl, "data-channel-id").toULongLong();
            if (cid != 0)
                shell.openChannel(currentAccountId, currentGuildId, Core::Snowflake(cid));
        } else if (action == QStringLiteral("channel-link")) {
            QString url = attribute(actionEl, "data-href");
            if (auto link = Discord::ChannelLink::parse(url)) {
                if (link->messageId.isValid())
                    shell.jumpToMessage(link->channelId, link->messageId);
                else
                    shell.openChannel(currentAccountId, link->guildId, link->channelId);
            }
        } else if (action == QStringLiteral("link")) {
            QString url = attribute(actionEl, "data-href");
            Rml::showConfirmModal(
                shell,
                tr("External Link"),
                QString(tr("Are you sure you want to open <b>%1</b>?")).arg(url.toHtmlEscaped()),
                tr("Open Link"),
                [url]() { QDesktopServices::openUrl(QUrl(url)); },
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

                    QString text;
                    if (textareaEl)
                        text = QString::fromStdString(textareaEl->GetValue()).trimmed();

                    if (text.isEmpty() && pendingAttachments.isEmpty())
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
                } else if (!pendingAttachments.isEmpty()) {
                    event.StopPropagation();
                    pendingAttachments.clear();
                    updateComposer();
                }
            } else if (key == ::Rml::Input::KI_UP) {
                QString currentVal;
                if (textareaEl)
                    currentVal = QString::fromStdString(textareaEl->GetValue());
                if (currentVal.isEmpty() && hasSendPermission) {
                    ChatModel *model = shell.chat();
                    Core::ClientInstance *instance = shell.currentInstance();
                    if (model && instance) {
                        Core::Snowflake selfId = instance->accountId();
                        for (int r = model->rowCount() - 1; r >= 0; --r) {
                            QModelIndex idx = model->index(r, 0);
                            if (Core::Snowflake(idx.data(ChatModel::UserIdRole).toULongLong()) == selfId &&
                                !idx.data(ChatModel::IsSystemMessageRole).toBool()) {
                                Core::Snowflake msgId(idx.data(ChatModel::MessageIdRole).toULongLong());
                                QString body = idx.data(ChatModel::ContentRole).toString();
                                event.StopPropagation();
                                setEditTarget(msgId, body);
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
    connect(&shell, &Shell::locationChanged, this, &ChatController::onLocationChanged);
    connect(&shell, &Shell::instanceChanged, this, &ChatController::onInstanceChanged);
    connect(&shell, &Shell::memberListVisibleChanged, this, [this](bool) { updateHeader(); });

    if (auto *chatModel = shell.chat()) {
        connect(chatModel, &QAbstractItemModel::rowsInserted, this, &ChatController::onRowsInserted);
        connect(chatModel, &QAbstractItemModel::rowsRemoved, this, &ChatController::onRowsRemoved);
        connect(chatModel, &QAbstractItemModel::dataChanged, this, &ChatController::onDataChanged);
        connect(chatModel, &QAbstractItemModel::modelReset, this, &ChatController::onModelReset);
        connect(chatModel, &ChatModel::atLatestChanged, this, &ChatController::onAtLatestChanged);
    }

    if (auto *typingTracker = shell.typing()) {
        connect(typingTracker, &Core::TypingTracker::typersChanged, this, &ChatController::onTypersChanged);
    }
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
    for (const auto &connection : std::as_const(instanceConnections))
        disconnect(connection);
    instanceConnections.clear();
    if (instance) {
        instanceConnections.append(connect(instance->presences(), &Core::PresenceManager::presencesChanged,
            this, [this](const QList<Core::Snowflake> &users) {
                if (users.contains(headerRecipientId))
                    updateHeader();
            }));
        instanceConnections.append(connect(instance, &Core::ClientInstance::channelUpdated,
            this, [this](const Discord::ChannelUpdate &channel) {
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

void ChatController::onRowsInserted(const QModelIndex &, int first, int last)
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
                el->SetInnerRML(buildMessageHtml(r).toStdString());
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
                el->SetInnerRML(buildMessageHtml(r).toStdString());
                messagesEl->AppendChild(std::move(el));
            }
        }

        requestRender();
        if (wasAtBottom)
            messagesEl->SetScrollTop(messagesEl->GetScrollHeight());
    }

    updateJumpBar();
}

void ChatController::onRowsRemoved(const QModelIndex &, int first, int last)
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

void ChatController::onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &)
{
    if (!messagesEl)
        return;

    for (int r = topLeft.row(); r <= bottomRight.row(); ++r) {
        if (r >= 0 && r < messagesEl->GetNumChildren()) {
            messagesEl->GetChild(r)->SetInnerRML(buildMessageHtml(r).toStdString());
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

    QString allRml;
    int count = model->rowCount();
    for (int r = 0; r < count; ++r) {
        allRml += QStringLiteral("<div>") + buildMessageHtml(r) + QStringLiteral("</div>");
    }

    messagesEl->SetInnerRML(allRml.toStdString());
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

QString ChatController::buildMessageHtml(int row)
{
    ChatModel *model = shell.chat();
    if (!model || row < 0 || row >= model->rowCount())
        return QString();

    QModelIndex idx = model->index(row, 0);
    Core::Snowflake msgId(idx.data(ChatModel::MessageIdRole).toULongLong());
    Core::Snowflake authorId(idx.data(ChatModel::UserIdRole).toULongLong());
    QString authorName = idx.data(ChatModel::UsernameRole).toString();
    QColor authorColor = idx.data(ChatModel::UsernameColorRole).value<QColor>();
    QDateTime timestamp = idx.data(ChatModel::TimestampRole).toDateTime();
    QVariant editedVal = idx.data(ChatModel::EditedTimestampRole);
    bool isPending = idx.data(ChatModel::IsPendingRole).toBool();
    bool isError = idx.data(ChatModel::IsErroredRole).toBool();
    bool isSystem = idx.data(ChatModel::IsSystemMessageRole).toBool();
    Discord::MessageType msgType = static_cast<Discord::MessageType>(idx.data(ChatModel::MessageTypeRole).toInt());
    ReplyData reply = idx.data(ChatModel::ReplyDataRole).value<ReplyData>();
    auto attachments = idx.data(ChatModel::AttachmentsRole).value<QList<AttachmentData>>();
    auto embeds = idx.data(ChatModel::EmbedsRole).value<QList<EmbedData>>();
    auto reactions = idx.data(ChatModel::ReactionsRole).value<QList<ReactionData>>();
    auto stickers = idx.data(ChatModel::StickersRole).value<QList<StickerData>>();
    QString rawContent = idx.data(ChatModel::ContentRole).toString();

    QString out;

    // Date separator check
    bool showDateSep = idx.data(ChatModel::DateSeparatorRole).toBool();
    if (showDateSep) {
        out += QStringLiteral(
            "<div class=\"date-separator\">"
            "<span class=\"date-separator-line\"></span>"
            "<span class=\"date-separator-text\">%1</span>"
            "<span class=\"date-separator-line\"></span>"
            "</div>"
        ).arg(formatDateSeparator(timestamp));
    }

    if (isSystem) {
        QString icon = systemMessageIcon(msgType);
        QString cls = systemMessageClass(msgType);
        QString content = rawContent.toHtmlEscaped();
        out += QStringLiteral(
            "<div class=\"system-message\">"
            "<img class=\"system-icon %1\" src=\"%2\" />"
            "<span class=\"system-text\"><strong>%3</strong> %4</span>"
            "<span class=\"system-timestamp\">%5</span>"
            "</div>"
        ).arg(cls, icon, authorName.toHtmlEscaped(), content, formatGutterTimestamp(timestamp));
        return out;
    }

    // 7-minute Discord grouping rule
    bool showHeader = true;
    if (row > 0) {
        QModelIndex prevIdx = model->index(row - 1, 0);
        Core::Snowflake prevAuthorId(prevIdx.data(ChatModel::UserIdRole).toULongLong());
        QDateTime prevTimestamp = prevIdx.data(ChatModel::TimestampRole).toDateTime();
        bool prevSystem = prevIdx.data(ChatModel::IsSystemMessageRole).toBool();

        if (!prevSystem &&
            prevAuthorId == authorId &&
            !showDateSep &&
            reply.state == ReplyData::State::None &&
            qAbs(prevTimestamp.secsTo(timestamp)) <= 7 * 60) {
            showHeader = false;
        }
    }

    // Parse Markdown content and check for user mention
    RenderedMarkdown contentRendered = ChatMarkdown::toRml(rawContent, shell, currentAccountId, currentGuildId, currentChannelId);
    bool mentionsMe = contentRendered.mentionsUser;

    QString rowClasses = QStringLiteral("message-row");
    if (showHeader)
        rowClasses += QStringLiteral(" group-start");
    if (mentionsMe)
        rowClasses += QStringLiteral(" mentions-me");
    if (isPending)
        rowClasses += QStringLiteral(" message-pending");
    if (isError)
        rowClasses += QStringLiteral(" message-error");

    out += QStringLiteral("<div class=\"%1\" data-message-id=\"%2\">").arg(rowClasses, QString::number(quint64(msgId)));

    // Reply preview
    if (reply.state != ReplyData::State::None) {
        Core::ClientInstance *instance = shell.currentInstance();
        QUrl refAvatarUrl;
        if (instance && instance->users() && reply.authorId.isValid()) {
            auto refUser = instance->users()->getUser(reply.authorId);
            refAvatarUrl = refUser ? instance->users()->getAvatarUrl(*refUser, currentGuildId, 32) : Discord::Cdn::defaultUserAvatar(reply.authorId, 32);
        } else {
            refAvatarUrl = Discord::Cdn::defaultUserAvatar(reply.authorId, 32);
        }
        QString refAvatarSrc = shell.imageSource(refAvatarUrl, QSize(16, 16));
        QString replyColorStyle = reply.authorColor.isValid() ? QStringLiteral("style=\"color:%1;\"").arg(reply.authorColor.name()) : QString();

        out += QStringLiteral(
            "<div class=\"reply-bar-container\" data-action=\"jump-to-message\" data-message-id=\"%1\">"
            "<div class=\"reply-spine\"></div>"
            "<div class=\"reply-avatar-clip\"><img class=\"reply-avatar\" src=\"%2\" /></div>"
            "<span class=\"reply-author\" %3>%4</span>"
            "<span class=\"reply-content\">%5</span>"
            "</div>"
        ).arg(QString::number(quint64(reply.referencedMessageId)),
             refAvatarSrc.toHtmlEscaped(),
             replyColorStyle,
             reply.authorName.toHtmlEscaped(),
             reply.contentSnippet.toHtmlEscaped());
    }

    // Header / Group start
    if (showHeader) {
        Core::ClientInstance *instance = shell.currentInstance();
        QUrl avatarUrl;
        if (instance && instance->users() && authorId.isValid()) {
            auto user = instance->users()->getUser(authorId);
            avatarUrl = user ? instance->users()->getAvatarUrl(*user, currentGuildId, 64) : Discord::Cdn::defaultUserAvatar(authorId, 64);
        } else {
            avatarUrl = Discord::Cdn::defaultUserAvatar(authorId, 64);
        }
        QString avatarSrc = shell.imageSource(avatarUrl, QSize(40, 40));
        QString authorColorStyle = authorColor.isValid() ? QStringLiteral("style=\"color:%1;\"").arg(authorColor.name()) : QString();

        out += QStringLiteral(
            "<div class=\"message-avatar-clip\" data-action=\"user-profile\" data-user-id=\"%2\"><img class=\"message-avatar\" src=\"%1\" /></div>"
            "<div class=\"message-header\">"
            "<span class=\"message-author\" %3 data-action=\"user-profile\" data-user-id=\"%2\">%4</span>"
            "<span class=\"message-timestamp\">%5</span>"
            "</div>"
        ).arg(avatarSrc.toHtmlEscaped(),
             QString::number(quint64(authorId)),
             authorColorStyle,
             authorName.toHtmlEscaped(),
             formatTimestamp(timestamp));
    } else {
        out += QStringLiteral("<span class=\"gutter-timestamp\">%1</span>").arg(formatGutterTimestamp(timestamp));
    }

    // Body content
    out += QStringLiteral("<div class=\"message-body\">");
    out += contentRendered.rml;
    if (editedVal.isValid()) {
        out += QStringLiteral("<span class=\"edited-marker\">(edited)</span>");
    }
    out += QStringLiteral("</div>");

    // Attachments
    for (const auto &att : attachments) {
        if (att.isMedia()) {
            if (att.isImage) {
                bool isSpoiler = att.isSpoiler && model && !model->isSpoilerRevealed(att.id);
                if (isSpoiler) {
                    out += QStringLiteral(
                        "<div class=\"attachment-spoiler-box\" data-action=\"reveal-attachment-spoiler\" data-attachment-id=\"%1\">"
                        "<span class=\"spoiler-pill\">SPOILER</span>"
                        "</div>"
                    ).arg(quint64(att.id));
                } else {
                    QSize disp = att.displaySize;
                    if (!disp.isValid())
                        disp = QSize(400, 300);
                    if (disp.width() > 400 || disp.height() > 300)
                        disp = disp.scaled(400, 300, Qt::KeepAspectRatio);

                    QString src = shell.imageSource(att.proxyUrl.isValid() ? att.proxyUrl : att.originalUrl, disp);
                    out += QStringLiteral(
                        "<div class=\"attachment-container\">"
                        "<img class=\"attachment-image\" src=\"%1\" data-action=\"open-image\" data-url=\"%2\" style=\"width:%3dp; height:%4dp;\" />"
                        "</div>"
                    ).arg(src.toHtmlEscaped(),
                         att.originalUrl.toString().toHtmlEscaped(),
                         QString::number(disp.width()),
                         QString::number(disp.height()));
                }
            } else if (att.isVideo) {
                out += QStringLiteral(
                    "<div class=\"attachment-file-card\" data-action=\"open-attachment\" data-url=\"%1\">"
                    "<img class=\"attachment-file-icon\" src=\"icon:image-play\" />"
                    "<div class=\"attachment-file-info\">"
                    "<span class=\"attachment-filename\">%2</span>"
                    "<span class=\"attachment-filesize\">%3</span>"
                    "</div>"
                    "</div>"
                ).arg(att.originalUrl.toString().toHtmlEscaped(),
                     att.filename.toHtmlEscaped(),
                     formatFileSize(att.fileSizeBytes));
            }
        } else {
            QString iconName = att.isAudio ? QStringLiteral("icon:file-audio") : QStringLiteral("icon:file-text");
            out += QStringLiteral(
                "<div class=\"attachment-file-card\" data-action=\"open-attachment\" data-url=\"%1\">"
                "<img class=\"attachment-file-icon\" src=\"%2\" />"
                "<div class=\"attachment-file-info\">"
                "<span class=\"attachment-filename\">%3</span>"
                "<span class=\"attachment-filesize\">%4</span>"
                "</div>"
                "</div>"
            ).arg(att.originalUrl.toString().toHtmlEscaped(),
                 iconName,
                 att.filename.toHtmlEscaped(),
                 formatFileSize(att.fileSizeBytes));
        }
    }

    // Embeds
    for (const auto &embed : embeds) {
        QString borderStyle = QStringLiteral("border-left: 4dp %1;").arg(embed.color.isValid() ? embed.color.name() : QStringLiteral("#202225"));
        out += QStringLiteral("<div class=\"embed-card\" style=\"%1\">").arg(borderStyle);

        if (!embed.authorName.isEmpty()) {
            out += QStringLiteral("<div class=\"embed-author\">");
            if (embed.authorIconUrl.isValid()) {
                QString iconSrc = shell.imageSource(embed.authorIconUrl, QSize(24, 24));
                out += QStringLiteral("<img class=\"embed-author-icon\" src=\"%1\" />").arg(iconSrc.toHtmlEscaped());
            }
            if (!embed.authorUrl.isEmpty()) {
                out += QStringLiteral("<a href=\"%1\" class=\"chat-link\" data-action=\"link\" data-href=\"%1\">%2</a>")
                    .arg(embed.authorUrl.toHtmlEscaped(), embed.authorName.toHtmlEscaped());
            } else {
                out += QStringLiteral("<span>%1</span>").arg(embed.authorName.toHtmlEscaped());
            }
            out += QStringLiteral("</div>");
        }

        if (!embed.title.isEmpty()) {
            if (!embed.url.isEmpty()) {
                out += QStringLiteral("<a href=\"%1\" class=\"embed-title\" data-action=\"link\" data-href=\"%1\">%2</a>")
                    .arg(embed.url.toHtmlEscaped(), embed.title.toHtmlEscaped());
            } else {
                out += QStringLiteral("<div class=\"embed-title\">%1</div>").arg(embed.title.toHtmlEscaped());
            }
        }

        if (!embed.description.isEmpty()) {
            RenderedMarkdown desc = ChatMarkdown::toRml(embed.description, shell, currentAccountId, currentGuildId, currentChannelId);
            out += QStringLiteral("<div class=\"embed-description\">");
            out += desc.rml;
            out += QStringLiteral("</div>");
        }

        if (!embed.fields.isEmpty()) {
            out += QStringLiteral("<div class=\"embed-fields\">");
            for (const auto &field : embed.fields) {
                out += QStringLiteral("<div class=\"embed-field%1\">").arg(field.isInline ? QString() : QStringLiteral(" non-inline"));
                out += QStringLiteral("<div class=\"embed-field-name\">%1</div>").arg(field.name.toHtmlEscaped());
                RenderedMarkdown fval = ChatMarkdown::toRml(field.value, shell, currentAccountId, currentGuildId, currentChannelId);
                out += QStringLiteral("<div class=\"embed-field-value\">");
                out += fval.rml;
                out += QStringLiteral("</div></div>");
            }
            out += QStringLiteral("</div>");
        }

        if (!embed.images.isEmpty() && embed.images.first().url.isValid()) {
            const auto &eimg = embed.images.first();
            QSize disp = eimg.displaySize;
            if (!disp.isValid() || disp.width() > 400 || disp.height() > 300)
                disp = disp.isValid() ? disp.scaled(400, 300, Qt::KeepAspectRatio) : QSize(400, 300);
            QString imgSrc = shell.imageSource(eimg.url, disp);
            out += QStringLiteral("<img class=\"embed-image\" src=\"%1\" data-action=\"open-image\" data-url=\"%2\" style=\"width:%3dp; height:%4dp;\" />")
                .arg(imgSrc.toHtmlEscaped(), eimg.url.toString().toHtmlEscaped(), QString::number(disp.width()), QString::number(disp.height()));
        } else if (embed.thumbnailUrl.isValid()) {
            QSize disp = embed.thumbnailSize;
            if (!disp.isValid() || disp.width() > 80 || disp.height() > 80)
                disp = QSize(80, 80);
            QString thumbSrc = shell.imageSource(embed.thumbnailUrl, disp);
            out += QStringLiteral("<img class=\"embed-image\" src=\"%1\" data-action=\"open-image\" data-url=\"%2\" style=\"width:%3dp; height:%4dp;\" />")
                .arg(thumbSrc.toHtmlEscaped(), embed.thumbnailUrl.toString().toHtmlEscaped(), QString::number(disp.width()), QString::number(disp.height()));
        }

        if (!embed.footerText.isEmpty()) {
            out += QStringLiteral("<div class=\"embed-footer\">");
            if (embed.footerIconUrl.isValid()) {
                QString ficonSrc = shell.imageSource(embed.footerIconUrl, QSize(20, 20));
                out += QStringLiteral("<img class=\"embed-footer-icon\" src=\"%1\" />").arg(ficonSrc.toHtmlEscaped());
            }
            out += QStringLiteral("<span>%1</span>").arg(embed.footerText.toHtmlEscaped());
            out += QStringLiteral("</div>");
        }

        out += QStringLiteral("</div>");
    }

    // Stickers
    for (const auto &sticker : stickers) {
        if (sticker.stillUrl.isValid() || sticker.animatedUrl.isValid()) {
            QUrl sUrl = sticker.stillUrl.isValid() ? sticker.stillUrl : sticker.animatedUrl;
            QString sSrc = shell.imageSource(sUrl, QSize(160, 160));
            out += QStringLiteral("<img class=\"sticker-image\" src=\"%1\" title=\"%2\" alt=\"%2\" />")
                .arg(sSrc.toHtmlEscaped(), sticker.name.toHtmlEscaped());
        }
    }

    // Reactions
    if (!reactions.isEmpty()) {
        out += QStringLiteral("<div class=\"reactions-container\">");
        for (const auto &rx : reactions) {
            QString emojiContent;
            if (rx.emojiId.isValid()) {
                QString eSrc = shell.imageSource(rx.emojiUrl, QSize(16, 16));
                emojiContent = QStringLiteral("<img src=\"%1\" alt=\":%2:\" />").arg(eSrc.toHtmlEscaped(), rx.emojiName.toHtmlEscaped());
            } else {
                emojiContent = richText(rx.emojiName);
            }
            out += QStringLiteral(
                "<div class=\"reaction-pill%1\" data-action=\"toggle-reaction\" data-message-id=\"%2\" data-emoji-name=\"%3\" data-emoji-id=\"%4\" data-emoji-animated=\"%5\" data-me=\"%6\" data-burst=\"%7\">"
                "%8"
                "<span class=\"reaction-count\">%9</span>"
                "</div>"
            ).arg(rx.me ? QStringLiteral(" me") : QString(),
                 QString::number(quint64(msgId)),
                 rx.emojiName.toHtmlEscaped(),
                 rx.emojiId.isValid() ? QString::number(quint64(rx.emojiId)) : QString(),
                 rx.emojiAnimated ? QStringLiteral("1") : QStringLiteral("0"),
                 rx.me ? QStringLiteral("1") : QStringLiteral("0"),
                 rx.isBurst ? QStringLiteral("1") : QStringLiteral("0"),
                 emojiContent,
                 QString::number(rx.count));
        }
        out += QStringLiteral("</div>");
    }

    // Hover toolbar
    out += QStringLiteral(
        "<div class=\"message-toolbar\">"
        "<div class=\"toolbar-button\" data-action=\"toolbar-react\" data-message-id=\"%1\" title=\"Add Reaction\"><img src=\"icon:smile-plus\" /></div>"
        "<div class=\"toolbar-button\" data-action=\"toolbar-reply\" data-message-id=\"%1\" data-author=\"%2\" title=\"Reply\"><img src=\"icon:reply\" /></div>"
        "<div class=\"toolbar-button\" data-action=\"toolbar-more\" data-message-id=\"%1\" title=\"More\"><img src=\"icon:ellipsis\" /></div>"
        "</div>"
    ).arg(QString::number(quint64(msgId)), authorName.toHtmlEscaped());

    out += QStringLiteral("</div>");
    return out;
}

QString ChatController::conversationName() const
{
    if (auto *tree = shell.channelTree()) {
        if (const auto *node = tree->findChannelTreeNode(currentChannelId, currentAccountId))
            return node->name;
    }
    if (auto *instance = shell.currentInstance()) {
        if (const auto channel = instance->getChannel(currentChannelId)) {
            if (channel->name.hasValue() && !channel->name->isEmpty())
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
    const QString name = conversationName();
    QString leadingIcon;
    QString presenceDot;
    QString topicHtml;

    if (isDm) {
        Core::Snowflake recipientId;
        QString avatarHash;
        if (auto *tree = shell.channelTree()) {
            if (const auto *node = tree->findChannelTreeNode(currentChannelId, currentAccountId)) {
                recipientId = node->dmRecipientId;
                avatarHash = node->dmAvatarHash;
            }
        }
        if (recipientId.isValid() && instance) {
            headerRecipientId = recipientId;
            const QUrl avatar = avatarHash.isEmpty()
                ? Discord::Cdn::defaultUserAvatar(recipientId)
                : Discord::Cdn::userAvatar(recipientId, avatarHash, 32);
            leadingIcon = QStringLiteral("<div class=\"header-avatar-clip\"><img src=\"%1\"/></div>")
                .arg(shell.imageSource(avatar, QSize(24, 24)));
            const auto badge = instance->presences()->badge(recipientId);
            QString status = QStringLiteral("status-offline");
            if (badge.streaming)
                status = QStringLiteral("status-streaming");
            else if (badge.status == Discord::StatusType::ONLINE)
                status = QStringLiteral("status-online");
            else if (badge.status == Discord::StatusType::IDLE)
                status = QStringLiteral("status-idle");
            else if (badge.status == Discord::StatusType::DND)
                status = QStringLiteral("status-dnd");
            presenceDot = QStringLiteral("<span class=\"header-presence %1\"></span>").arg(status);
        } else {
            leadingIcon = QStringLiteral("<img class=\"header-icon\" src=\"icon:at-sign\"/>");
        }
    } else {
        QString icon = QStringLiteral("icon:hash");
        if (instance) {
            if (const auto channel = instance->getChannel(currentChannelId)) {
                if (channel->isVoice())
                    icon = QStringLiteral("icon:volume-2");
                else if (channel->type.valueOr(Discord::ChannelType::GUILD_TEXT) == Discord::ChannelType::GUILD_NEWS)
                    icon = QStringLiteral("icon:megaphone");
                if (channel->topic.hasValue() && !channel->topic->isEmpty()) {
                    const auto topic = ChatMarkdown::toRml(channel->topic->simplified(), shell,
                        currentAccountId, currentGuildId, currentChannelId);
                    topicHtml = QStringLiteral("<span class=\"header-divider\"></span><div class=\"header-topic\">")
                        + topic.rml + QStringLiteral("</div>");
                }
            }
        }
        leadingIcon = QStringLiteral("<img class=\"header-icon\" src=\"%1\"/>").arg(icon);
    }

    const QString threads = isDm ? QString() : QStringLiteral(
        "<div class=\"header-btn\" data-action=\"open-threads\" title=\"Threads\"><img src=\"icon:messages-square\"/></div>");
    const QString members = QStringLiteral(
        "<div class=\"header-btn%1\" data-action=\"toggle-members\" title=\"%2\"><img src=\"icon:id-card\"/></div>")
        .arg(shell.isMemberListVisible() ? QStringLiteral(" active") : QString(),
             isDm ? QStringLiteral("Conversation profile") : QStringLiteral("Member list"));
    const QString html = QStringLiteral("<div class=\"header-left\">") + leadingIcon
        + QStringLiteral("<span class=\"header-title\">") + escape(name) + QStringLiteral("</span>")
        + presenceDot + topicHtml + QStringLiteral("</div><div class=\"header-right\">")
        + threads + members + QStringLiteral("</div>");
    headerEl->SetInnerRML(html.toStdString());
    requestRender();
}

void ChatController::updateTyping()
{
    if (!typingEl)
        return;

    auto typers = shell.typing() ? shell.typing()->getActiveTypers() : QList<Core::TyperInfo>();
    if (typers.isEmpty()) {
        typingEl->SetInnerRML("");
        requestRender();
        return;
    }

    QString text;
    if (typers.size() == 1) {
        text = QStringLiteral("<b>%1</b> is typing…").arg(typers[0].name.toHtmlEscaped());
    } else if (typers.size() == 2) {
        text = QStringLiteral("<b>%1</b> and <b>%2</b> are typing…")
            .arg(typers[0].name.toHtmlEscaped(), typers[1].name.toHtmlEscaped());
    } else if (typers.size() == 3) {
        text = QStringLiteral("<b>%1</b>, <b>%2</b>, and <b>%3</b> are typing…")
            .arg(typers[0].name.toHtmlEscaped(), typers[1].name.toHtmlEscaped(), typers[2].name.toHtmlEscaped());
    } else {
        text = QStringLiteral("Several people are typing…");
    }

    QString html = QStringLiteral(
        "<div class=\"typing-dots\">"
        "<span class=\"typing-dot\"></span>"
        "<span class=\"typing-dot\"></span>"
        "<span class=\"typing-dot\"></span>"
        "</div>"
        "<span class=\"typing-text\">%1</span>"
    ).arg(text);

    typingEl->SetInnerRML(html.toStdString());
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
    QString placeholder = QStringLiteral("Message...");

    if (instance) {
        auto ch = instance->getChannel(currentChannelId);
        bool isDm = shell.location().isHome();
        const QString chName = conversationName();

        if (isDm) {
            placeholder = QStringLiteral("Message @") + chName;
            hasSendPermission = true;
        } else {
            placeholder = QStringLiteral("Message #") + chName;
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
        placeholder = QStringLiteral("You do not have permission to send messages in this channel.");

    // Reply bar HTML
    QString replyBarHtml;
    if (replyTargetId.isValid()) {
        replyBarHtml = QStringLiteral(
            "<div class=\"composer-reply-bar\">"
            "<div class=\"composer-reply-text\">Replying to <strong>%1</strong> <span>%2</span></div>"
            "<img class=\"composer-reply-close\" src=\"icon:x\" data-action=\"composer-reply-close\" title=\"Cancel reply\" />"
            "</div>"
        ).arg(replyAuthorName.toHtmlEscaped(), replySnippet.toHtmlEscaped());
    } else if (editingMessageId.isValid()) {
        replyBarHtml = QStringLiteral(
            "<div class=\"composer-reply-bar\">"
            "<div class=\"composer-reply-text\">Editing message — <span>Escape to cancel</span></div>"
            "<img class=\"composer-reply-close\" src=\"icon:x\" data-action=\"composer-reply-close\" title=\"Cancel edit\" />"
            "</div>"
        );
    }

    // Attachments bar HTML
    QString attachmentsHtml;
    if (!pendingAttachments.isEmpty()) {
        attachmentsHtml = QStringLiteral("<div class=\"composer-attachments-bar\">");
        for (int i = 0; i < pendingAttachments.size(); ++i) {
            const auto &att = pendingAttachments[i];
            attachmentsHtml += QStringLiteral(
                "<div class=\"composer-attachment-item\">"
                "<span>%1 (%2)</span>"
                "<img class=\"composer-attachment-remove\" src=\"icon:x\" data-action=\"composer-remove-attachment\" data-index=\"%3\" title=\"Remove\" />"
                "</div>"
            ).arg(att.filename.toHtmlEscaped(), formatFileSize(att.size), QString::number(i));
        }
        attachmentsHtml += QStringLiteral("</div>");
    }

    // Slowmode notice HTML
    QString slowmodeHtml;
    if (channelRateLimit > 0) {
        slowmodeHtml = QStringLiteral(
            "<div class=\"slowmode-notice\"><img src=\"icon:clock\" style=\"width:14dp;height:14dp;image-color:#f0b232;\" /> Slowmode is active (%1s)</div>"
        ).arg(channelRateLimit);
    }

    QString composerHtml = QStringLiteral(
        "%1"
        "%2"
        "<div class=\"composer-main-row\">"
        "<div class=\"composer-btn\" data-action=\"composer-attach\" title=\"Attach file\"><img src=\"icon:image\" /></div>"
        "<div class=\"composer-input-wrap\">"
        "<span id=\"composer-placeholder\" class=\"composer-placeholder\">%4</span>"
        "<textarea id=\"composer-input\" class=\"composer-textarea%3\" rows=\"1\"%5></textarea>"
        "</div>"
        "<div class=\"composer-btn\" data-action=\"composer-emoji\" title=\"Select emoji\"><img src=\"icon:smile\" /></div>"
        "</div>"
        "%6"
    ).arg(replyBarHtml,
         attachmentsHtml,
         hasSendPermission ? QString() : QStringLiteral(" composer-disabled"),
         placeholder.toHtmlEscaped(),
         hasSendPermission ? QString() : QStringLiteral(" disabled"),
         slowmodeHtml);

    textareaEl = nullptr;
    composerEl->SetInnerRML(composerHtml.toStdString());
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

void ChatController::setReplyTarget(Core::Snowflake messageId, const QString &authorName, const QString &snippet)
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

void ChatController::setEditTarget(Core::Snowflake messageId, const QString &content)
{
    editingMessageId = messageId;
    replyTargetId = Core::Snowflake::Invalid;
    updateComposer();
    if (textareaEl) {
        textareaEl->SetValue(content.toStdString());
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

    QModelIndex idx = model->index(row, 0);
    Core::Snowflake authorId(idx.data(ChatModel::UserIdRole).toULongLong());
    QString content = idx.data(ChatModel::ContentRole).toString();
    Core::ClientInstance *instance = shell.currentInstance();
    bool isSelf = (instance && authorId == instance->accountId());

    const auto accountId = currentAccountId;
    const auto channelId = currentChannelId;
    const auto guildId = currentGuildId;
    QPointer<Core::ClientInstance> instanceGuard(instance);
    const QString author = idx.data(ChatModel::UsernameRole).toString();
    std::vector<MenuItem> menu;
    menu.push_back({ tr("Add Reaction"), [this, accountId, channelId, messageId] {
        shell.openExpressionPickerForReaction(accountId, channelId, messageId, QRect(QCursor::pos(), QSize(1, 1)));
    } });
    menu.push_back({ tr("Reply"), [this, messageId, author, content, accountId, channelId] {
        if (currentAccountId == accountId && currentChannelId == channelId)
            setReplyTarget(messageId, author, content.left(50));
    } });
    if (isSelf) {
        menu.push_back({ tr("Edit Message"), [this, messageId, content, accountId, channelId] {
            if (currentAccountId == accountId && currentChannelId == channelId)
                setEditTarget(messageId, content);
        } });
        menu.push_back({ tr("Delete Message"), [this, messageId, instanceGuard, channelId] {
            if (instanceGuard) {
                showConfirmModal(shell, tr("Delete Message"),
                    tr("Are you sure you want to delete this message?"), tr("Delete"),
                    [instanceGuard, channelId, messageId] {
                        if (instanceGuard)
                            instanceGuard->discord()->deleteMessage(channelId, messageId);
                    }, nullptr, true);
            }
        }, true, true });
    }
    menu.push_back({ {}, {}, false, false, true });
    menu.push_back({ tr("Copy Text"), [content] { QGuiApplication::clipboard()->setText(content); } });
    menu.push_back({ tr("Copy Message Link"), [guildId, channelId, messageId] {
        const QString guildPart = guildId.isValid() ? QString::number(quint64(guildId)) : QStringLiteral("@me");
        QGuiApplication::clipboard()->setText(QStringLiteral("https://discord.com/channels/%1/%2/%3")
            .arg(guildPart, QString::number(quint64(channelId)), QString::number(quint64(messageId))));
    } });
    menu.push_back({ tr("Copy ID"), [messageId] {
        QGuiApplication::clipboard()->setText(QString::number(quint64(messageId)));
    } });
    shell.showContextMenu(std::move(menu));
}

void ChatController::openReactionPickerForMessage(Core::Snowflake messageId)
{
    shell.openExpressionPickerForReaction(currentAccountId, currentChannelId, messageId, QRect(QCursor::pos(), QSize(1, 1)));
}

void ChatController::openEmojiPickerForComposer()
{
    if (auto *picker = shell.expressionPicker()) {
        disconnect(picker, &ExpressionPickerController::emojiPicked, this, nullptr);
        disconnect(picker, &ExpressionPickerController::gifPicked, this, nullptr);

        connect(picker, &ExpressionPickerController::emojiPicked, this, [this](const Core::PickerEmoji &emoji, bool) {
            if (textareaEl) {
                QString current = QString::fromStdString(textareaEl->GetValue());
                QString toInsert = emoji.messageText();
                textareaEl->SetValue((current + toInsert).toStdString());
                updateComposerPlaceholder();
                textareaEl->Focus();
            }
        });

        connect(picker, &ExpressionPickerController::gifPicked, this, [this](const QString &url) {
            shell.sendMessage(url);
        });
    }

    shell.openExpressionPickerForChat(currentAccountId, currentChannelId, QRect(QCursor::pos(), QSize(1, 1)));
}

} // namespace Rml
} // namespace UI
} // namespace Styx