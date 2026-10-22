#include "ForumController.hpp"

#include "Shell.hpp"
#include "RmlRuntime.hpp"
#include "RmlView.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/ImageManager.hpp"
#include "Core/ReadStateManager.hpp"
#include "Core/TimeUtils.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

namespace Styx {
namespace UI {
namespace Rml {

static QString escape(const QString &str)
{
    return str.toHtmlEscaped();
}

ForumController::ForumController(Shell &shell, QObject *parent)
    : Controller(shell, nullptr, parent)
{
}

ForumController::~ForumController()
{
    for (auto &conn : managerConnections)
        QObject::disconnect(conn);
    managerConnections.clear();

    if (forumDocument) {
        forumDocument->Close();
        forumDocument = nullptr;
    }
}

void ForumController::ensureDocument()
{
    if (forumDocument)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    forumDocument = view->context()->LoadDocument(":/ui/forum.rml");
    if (!forumDocument)
        return;

    regionRoot = forumDocument;
    setupEventListeners();
}

void ForumController::syncFolderState()
{
    if (!forumDocument)
        return;
    const bool railFolderOpen = shell.document() && shell.document()->IsClassSet("rail-folder-open");
    forumDocument->SetClass("rail-folder-open", railFolderOpen);
}

void ForumController::open(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake forumId)
{
    ensureDocument();
    if (!forumDocument)
        return;

    for (auto &conn : managerConnections)
        QObject::disconnect(conn);
    managerConnections.clear();

    currentAccountId = accountId;
    currentGuildId = guildId;
    currentForumId = forumId;
    selectedTagId = Core::Snowflake::Invalid;

    auto *inst = shell.instanceFor(accountId);
    if (!inst)
        return;

    auto *mgr = inst->forums();
    if (mgr) {
        currentSortMode = mgr->sortMode(forumId);
        managerConnections.append(connect(mgr, &Core::ForumManager::postsReset, this, &ForumController::onPostsReset));
        managerConnections.append(connect(mgr, &Core::ForumManager::postsAppended, this, &ForumController::onPostsAppended));
        managerConnections.append(connect(mgr, &Core::ForumManager::postInserted, this, &ForumController::onPostInserted));
        managerConnections.append(connect(mgr, &Core::ForumManager::postUpdated, this, &ForumController::onPostUpdated));
        managerConnections.append(connect(mgr, &Core::ForumManager::postRemoved, this, &ForumController::onPostRemoved));
        managerConnections.append(connect(mgr, &Core::ForumManager::loadingChanged, this, &ForumController::onLoadingChanged));
        mgr->openForum(forumId);
    }

    visible = true;
    syncFolderState();
    forumDocument->Show(::Rml::ModalFlag::None);

    updateHeader();
    updateControls();
    updateTags();
    renderPosts();
    updateLoadingState();
    requestRender();
}

void ForumController::close()
{
    if (!visible)
        return;

    visible = false;
    currentForumId = Core::Snowflake::Invalid;

    for (auto &conn : managerConnections)
        QObject::disconnect(conn);
    managerConnections.clear();

    if (forumDocument)
        forumDocument->Hide();

    requestRender();
}

void ForumController::setupEventListeners()
{
    if (!forumDocument)
        return;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        auto *actionEl = closest(target, "data-action");
        if (!actionEl)
            return;

        const QString action = attribute(actionEl, "data-action");

        if (action == QStringLiteral("sort-recent")) {
            auto *inst = shell.instanceFor(currentAccountId);
            if (inst && inst->forums() && currentForumId.isValid()) {
                currentSortMode = Core::ForumSortMode::LATEST_ACTIVITY;
                inst->forums()->setSortMode(currentForumId, currentSortMode);
                updateControls();
                renderPosts();
            }
        } else if (action == QStringLiteral("sort-created")) {
            auto *inst = shell.instanceFor(currentAccountId);
            if (inst && inst->forums() && currentForumId.isValid()) {
                currentSortMode = Core::ForumSortMode::CREATION_DATE;
                inst->forums()->setSortMode(currentForumId, currentSortMode);
                updateControls();
                renderPosts();
            }
        } else if (action == QStringLiteral("open-composer")) {
            if (auto *composer = forumDocument->GetElementById("forum-composer"))
                composer->SetProperty("display", "flex");
            requestRender();
        } else if (action == QStringLiteral("close-composer")) {
            if (auto *composer = forumDocument->GetElementById("forum-composer"))
                composer->SetProperty("display", "none");
            requestRender();
        } else if (action == QStringLiteral("filter-all")) {
            selectedTagId = Core::Snowflake::Invalid;
            updateTags();
            renderPosts();
        } else if (action == QStringLiteral("filter-tag")) {
            const quint64 tid = attribute(actionEl, "data-tag-id").toULongLong();
            if (selectedTagId == Core::Snowflake(tid))
                selectedTagId = Core::Snowflake::Invalid;
            else
                selectedTagId = Core::Snowflake(tid);
            updateTags();
            renderPosts();
        } else if (action == QStringLiteral("load-more")) {
            auto *inst = shell.instanceFor(currentAccountId);
            if (inst && inst->forums() && currentForumId.isValid())
                inst->forums()->loadMorePosts(currentForumId);
        } else if (action == QStringLiteral("open-post")) {
            const quint64 threadIdVal = attribute(actionEl, "data-thread-id").toULongLong();
            if (threadIdVal != 0) {
                Core::Snowflake threadId(threadIdVal);
                close();
                shell.openChannel(currentAccountId, currentGuildId, threadId);
            }
        }
    });
}

void ForumController::updateHeader()
{
    if (!forumDocument)
        return;

    auto *inst = shell.instanceFor(currentAccountId);
    QString name = QStringLiteral("Forum");
    QString topic;

    if (inst) {
        if (auto ch = inst->getChannel(currentForumId)) {
            if (ch->name.hasValue() && !ch->name->isEmpty())
                name = ch->name.get();
            if (ch->topic.hasValue() && !ch->topic->isEmpty())
                topic = ch->topic.get();
        }
    }

    if (auto *titleEl = forumDocument->GetElementById("forum-title"))
        titleEl->SetInnerRML(escape(name).toStdString());

    if (auto *topicEl = forumDocument->GetElementById("forum-topic")) {
        if (topic.isEmpty()) {
            topicEl->SetProperty("display", "none");
            if (auto *divEl = forumDocument->GetElementById("forum-divider"))
                divEl->SetProperty("display", "none");
        } else {
            topicEl->SetProperty("display", "block");
            topicEl->SetInnerRML(escape(topic).toStdString());
            if (auto *divEl = forumDocument->GetElementById("forum-divider"))
                divEl->SetProperty("display", "block");
        }
    }
}

void ForumController::updateControls()
{
    if (!forumDocument)
        return;

    auto *recentBtn = forumDocument->GetElementById("forum-sort-recent");
    auto *createdBtn = forumDocument->GetElementById("forum-sort-created");

    if (recentBtn && createdBtn) {
        recentBtn->SetClass("active", currentSortMode == Core::ForumSortMode::LATEST_ACTIVITY);
        createdBtn->SetClass("active", currentSortMode == Core::ForumSortMode::CREATION_DATE);
    }
}

void ForumController::updateTags()
{
    if (!forumDocument)
        return;

    auto *tagsBar = forumDocument->GetElementById("forum-tags-bar");
    if (!tagsBar)
        return;

    auto *inst = shell.instanceFor(currentAccountId);
    if (!inst || !inst->forums()) {
        tagsBar->SetInnerRML("");
        return;
    }

    const auto tags = inst->forums()->availableTags(currentForumId);
    if (tags.isEmpty()) {
        tagsBar->SetInnerRML("");
        return;
    }

    QString rml;
    const bool allActive = !selectedTagId.isValid();
    rml += QStringLiteral("<div class=\"forum-tag-chip%1\" data-action=\"filter-all\">All</div>")
               .arg(allActive ? QStringLiteral(" active") : QString());

    for (const auto &tag : tags) {
        if (!tag.id.hasValue() || !tag.name.hasValue())
            continue;

        const Core::Snowflake tagId = tag.id.get();
        const bool isActive = (selectedTagId == tagId);

        QString iconPart;
        if (tag.emojiId.hasValue() && tag.emojiId.get().isValid()) {
            QUrl emojiUrl = Discord::Cdn::emoji(tag.emojiId.get(), 20);
            iconPart = QStringLiteral("<img class=\"tag-emoji\" src=\"%1\"/>")
                           .arg(escape(shell.imageSource(emojiUrl, QSize(14, 14))));
        } else if (tag.emojiName.hasValue() && !tag.emojiName->isEmpty()) {
            iconPart = QStringLiteral("<span class=\"tag-emoji\">%1</span>").arg(escape(tag.emojiName.get()));
        }

        rml += QStringLiteral("<div class=\"forum-tag-chip%1\" data-action=\"filter-tag\" data-tag-id=\"%2\">%3%4</div>")
                   .arg(isActive ? QStringLiteral(" active") : QString(),
                        QString::number(static_cast<quint64>(tagId)),
                        iconPart,
                        escape(tag.name.get()));
    }

    tagsBar->SetInnerRML(rml.toStdString());
}

QUrl ForumController::thumbnailUrlOf(const Discord::Message *msg)
{
    if (!msg)
        return {};

    if (msg->attachments.hasValue()) {
        for (const auto &att : msg->attachments.get()) {
            if (!att.isImage())
                continue;
            if (att.proxyUrl.hasValue() && !att.proxyUrl.get().isEmpty())
                return QUrl(att.proxyUrl.get());
            if (att.url.hasValue() && !att.url.get().isEmpty())
                return QUrl(att.url.get());
        }
    }

    if (msg->embeds.hasValue()) {
        for (const auto &embed : msg->embeds.get()) {
            if (embed.image.hasValue()) {
                const auto &img = embed.image.get();
                if (img.proxyUrl.hasValue() && !img.proxyUrl.get().isEmpty())
                    return QUrl(img.proxyUrl.get());
                if (img.url.hasValue() && !img.url.get().isEmpty())
                    return QUrl(img.url.get());
            }
            if (embed.thumbnail.hasValue()) {
                const auto &thumb = embed.thumbnail.get();
                if (thumb.proxyUrl.hasValue() && !thumb.proxyUrl.get().isEmpty())
                    return QUrl(thumb.proxyUrl.get());
                if (thumb.url.hasValue() && !thumb.url.get().isEmpty())
                    return QUrl(thumb.url.get());
            }
        }
    }

    return {};
}

QString ForumController::buildPostCardHtml(const Discord::Channel &post)
{
    const Core::Snowflake threadId = post.id.get();
    auto *inst = shell.instanceFor(currentAccountId);
    if (!inst || !inst->forums())
        return {};

    auto *mgr = inst->forums();
    mgr->ensureStarter(currentForumId, threadId);
    const Discord::Message *starter = mgr->firstMessagePtr(threadId);

    const auto readState = mgr->postReadState(threadId);
    const QString title = post.name.valueOr(QStringLiteral("Untitled"));

    // Tags lookup
    QString tagsHtml;
    if (post.appliedTags.hasValue() && !post.appliedTags->isEmpty()) {
        const auto available = mgr->availableTags(currentForumId);
        QHash<Core::Snowflake, Discord::ForumTag> tagLookup;
        for (const auto &t : available) {
            if (t.id.hasValue())
                tagLookup.insert(t.id.get(), t);
        }

        QString pills;
        for (const auto &tId : post.appliedTags.get()) {
            auto it = tagLookup.constFind(tId);
            if (it == tagLookup.constEnd() || !it->name.hasValue())
                continue;

            QString iconPart;
            if (it->emojiId.hasValue() && it->emojiId.get().isValid()) {
                QUrl emojiUrl = Discord::Cdn::emoji(it->emojiId.get(), 20);
                iconPart = QStringLiteral("<img class=\"tag-emoji\" src=\"%1\"/>")
                               .arg(escape(shell.imageSource(emojiUrl, QSize(12, 12))));
            } else if (it->emojiName.hasValue() && !it->emojiName->isEmpty()) {
                iconPart = QStringLiteral("<span class=\"tag-emoji\">%1</span>").arg(escape(it->emojiName.get()));
            }

            pills += QStringLiteral("<div class=\"forum-card-tag-pill\">%1%2</div>")
                         .arg(iconPart, escape(it->name.get()));
        }

        if (!pills.isEmpty())
            tagsHtml = QStringLiteral("<div class=\"forum-card-tags-row\">%1</div>").arg(pills);
    }

    // Message snippet
    QString snippet;
    if (starter && starter->content.hasValue() && !starter->content->isEmpty()) {
        snippet = starter->content.get().simplified();
        if (snippet.length() > 140)
            snippet = snippet.left(137) + QStringLiteral("…");
    }

    // Author info
    QString authorName = QStringLiteral("User");
    QString avatarSrc;
    QColor authorColor;

    if (starter && starter->author.hasValue()) {
        const auto &author = starter->author.get();
        if (author.id.hasValue()) {
            const Core::Snowflake authorId = author.id.get();
            authorColor = shell.roleColor(authorId, currentGuildId);
            QUrl avatarUrl = Discord::Cdn::userAvatar(authorId, author.avatar.valueOr(QString()), 32);
            avatarSrc = shell.imageSource(avatarUrl, QSize(18, 18));
        }
        if (author.globalName.hasValue() && !author.globalName->isEmpty())
            authorName = author.globalName.get();
        else if (author.username.hasValue())
            authorName = author.username.get();
    }

    QString authorStyle;
    if (authorColor.isValid())
        authorStyle = QStringLiteral(" style=\"color: %1;\"").arg(authorColor.name());

    QString avatarHtml;
    if (!avatarSrc.isEmpty())
        avatarHtml = QStringLiteral("<img class=\"forum-card-avatar\" src=\"%1\"/>").arg(escape(avatarSrc));

    // Time
    const QDateTime dt = post.effectiveLastMessageId().toDateTime();
    const QString relTime = Core::TimeUtils::relativeTime(dt);

    // Reply count
    const int replyCount = post.messageCount.valueOr(0);

    // Thumbnail
    QUrl thumbUrl = thumbnailUrlOf(starter);
    QString thumbHtml;
    if (thumbUrl.isValid()) {
        QString imgSrc = shell.imageSource(thumbUrl, QSize(80, 80));
        thumbHtml = QStringLiteral(
            "<div class=\"forum-card-thumbnail-container\">"
            "<img class=\"forum-card-thumbnail\" src=\"%1\"/>"
            "</div>").arg(escape(imgSrc));
    }

    QString unreadDot;
    if (readState.unread)
        unreadDot = QStringLiteral("<div class=\"forum-card-unread-dot\"></div>");

    return QStringLiteral(
        "<div class=\"forum-card\" data-action=\"open-post\" data-thread-id=\"%1\">"
        "  <div class=\"forum-card-left\">"
        "    <div class=\"forum-card-top-row\">"
        "      %2"
        "      <span class=\"forum-card-title%3\">%4</span>"
        "    </div>"
        "    %5"
        "    <div class=\"forum-card-snippet\">%6</div>"
        "    <div class=\"forum-card-footer\">"
        "      <div class=\"forum-card-meta-left\">"
        "        %7"
        "        <span class=\"forum-card-author\"%8>%9</span>"
        "        <span class=\"forum-card-time\">%10</span>"
        "      </div>"
        "      <div class=\"forum-card-meta-right\">"
        "        <div class=\"forum-card-replies\">"
        "          <img src=\"icon:message-circle\"/>"
        "          <span>%11</span>"
        "        </div>"
        "      </div>"
        "    </div>"
        "  </div>"
        "  %12"
        "</div>")
        .arg(QString::number(static_cast<quint64>(threadId)),
             unreadDot,
             readState.unread ? QStringLiteral(" unread") : QString(),
             escape(title),
             tagsHtml,
             escape(snippet),
             avatarHtml,
             authorStyle,
             escape(authorName),
             escape(relTime),
             QString::number(replyCount),
             thumbHtml);
}

void ForumController::renderPosts()
{
    if (!forumDocument)
        return;

    auto *postsList = forumDocument->GetElementById("forum-posts-list");
    auto *emptyMsg = forumDocument->GetElementById("forum-empty");
    auto *loadMoreRow = forumDocument->GetElementById("forum-load-more");

    if (!postsList)
        return;

    auto *inst = shell.instanceFor(currentAccountId);
    if (!inst || !inst->forums()) {
        postsList->SetInnerRML("");
        if (emptyMsg)
            emptyMsg->SetProperty("display", "block");
        if (loadMoreRow)
            loadMoreRow->SetProperty("display", "none");
        return;
    }

    auto *mgr = inst->forums();
    const auto &posts = mgr->posts(currentForumId);

    QString rml;
    int visibleCount = 0;

    for (const auto &post : posts) {
        if (selectedTagId.isValid()) {
            if (!post.appliedTags.hasValue() || !post.appliedTags->contains(selectedTagId))
                continue;
        }
        rml += buildPostCardHtml(post);
        ++visibleCount;
    }

    postsList->SetInnerRML(rml.toStdString());

    if (emptyMsg)
        emptyMsg->SetProperty("display", visibleCount == 0 && !mgr->isLoading(currentForumId) ? "block" : "none");

    if (loadMoreRow)
        loadMoreRow->SetProperty("display", mgr->hasMore(currentForumId) ? "flex" : "none");
}

void ForumController::updateLoadingState()
{
    if (!forumDocument)
        return;

    auto *loadingMsg = forumDocument->GetElementById("forum-loading");
    if (!loadingMsg)
        return;

    auto *inst = shell.instanceFor(currentAccountId);
    const bool isLoading = (inst && inst->forums() && inst->forums()->isLoading(currentForumId));
    loadingMsg->SetProperty("display", isLoading ? "block" : "none");
}

void ForumController::onPostsReset(Core::Snowflake forumId)
{
    if (forumId == currentForumId) {
        renderPosts();
        updateLoadingState();
        requestRender();
    }
}

void ForumController::onPostsAppended(Core::Snowflake forumId, int, int)
{
    if (forumId == currentForumId) {
        renderPosts();
        updateLoadingState();
        requestRender();
    }
}

void ForumController::onPostInserted(Core::Snowflake forumId, Core::Snowflake, int)
{
    if (forumId == currentForumId) {
        renderPosts();
        requestRender();
    }
}

void ForumController::onPostUpdated(Core::Snowflake forumId, Core::Snowflake)
{
    if (forumId == currentForumId) {
        renderPosts();
        requestRender();
    }
}

void ForumController::onPostRemoved(Core::Snowflake forumId, Core::Snowflake)
{
    if (forumId == currentForumId) {
        renderPosts();
        requestRender();
    }
}

void ForumController::onLoadingChanged(Core::Snowflake forumId, bool)
{
    if (forumId == currentForumId) {
        updateLoadingState();
        renderPosts();
        requestRender();
    }
}

} // namespace Rml
} // namespace UI
} // namespace Styx
