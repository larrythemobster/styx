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
#include "Platform/Native.hpp"
#include <algorithm>
#include <charconv>
#include <format>


#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

namespace Styx {
namespace UI {
namespace Rml {

static std::string escape(const std::string &str)
{
    return Platform::htmlEscape(str);
}

ForumController::ForumController(Shell &shell)
    : Controller(shell, nullptr)
{
}

ForumController::~ForumController()
{
    managerConnections.clear();
    removeListeners();

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

    forumDocument = view->context()->LoadDocument("ui/forum.rml");
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
        managerConnections.push_back(mgr->postsReset.connect([this](auto id) { onPostsReset(id); }));
        managerConnections.push_back(mgr->postsAppended.connect([this](auto id, int start, int count) { onPostsAppended(id, start, count); }));
        managerConnections.push_back(mgr->postInserted.connect([this](auto id, auto thread, int row) { onPostInserted(id, thread, row); }));
        managerConnections.push_back(mgr->postUpdated.connect([this](auto id, auto thread) { onPostUpdated(id, thread); }));
        managerConnections.push_back(mgr->postRemoved.connect([this](auto id, auto thread) { onPostRemoved(id, thread); }));
        managerConnections.push_back(mgr->loadingChanged.connect([this](auto id, bool loading) { onLoadingChanged(id, loading); }));
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

        const std::string action = attribute(actionEl, "data-action");

        if (action == "sort-recent") {
            auto *inst = shell.instanceFor(currentAccountId);
            if (inst && inst->forums() && currentForumId.isValid()) {
                currentSortMode = Core::ForumSortMode::LATEST_ACTIVITY;
                inst->forums()->setSortMode(currentForumId, currentSortMode);
                updateControls();
                renderPosts();
            }
        } else if (action == "sort-created") {
            auto *inst = shell.instanceFor(currentAccountId);
            if (inst && inst->forums() && currentForumId.isValid()) {
                currentSortMode = Core::ForumSortMode::CREATION_DATE;
                inst->forums()->setSortMode(currentForumId, currentSortMode);
                updateControls();
                renderPosts();
            }
        } else if (action == "open-composer") {
            if (auto *composer = forumDocument->GetElementById("forum-composer"))
                composer->SetProperty("display", "flex");
            requestRender();
        } else if (action == "close-composer") {
            if (auto *composer = forumDocument->GetElementById("forum-composer"))
                composer->SetProperty("display", "none");
            requestRender();
        } else if (action == "filter-all") {
            selectedTagId = Core::Snowflake::Invalid;
            updateTags();
            renderPosts();
        } else if (action == "filter-tag") {
            const auto text = attribute(actionEl, "data-tag-id");
            std::uint64_t tid = 0;
            std::from_chars(text.data(), text.data() + text.size(), tid);
            if (selectedTagId == Core::Snowflake(tid))
                selectedTagId = Core::Snowflake::Invalid;
            else
                selectedTagId = Core::Snowflake(tid);
            updateTags();
            renderPosts();
        } else if (action == "load-more") {
            auto *inst = shell.instanceFor(currentAccountId);
            if (inst && inst->forums() && currentForumId.isValid())
                inst->forums()->loadMorePosts(currentForumId);
        } else if (action == "open-post") {
            const auto text = attribute(actionEl, "data-thread-id");
            std::uint64_t threadIdVal = 0;
            std::from_chars(text.data(), text.data() + text.size(), threadIdVal);
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
    std::string name = "Forum";
    std::string topic;

    if (inst) {
        if (auto ch = inst->getChannel(currentForumId)) {
            if (ch->name.hasValue() && !ch->name->empty())
                name = ch->name.get();
            if (ch->topic.hasValue() && !ch->topic->empty())
                topic = ch->topic.get();
        }
    }

    if (auto *titleEl = forumDocument->GetElementById("forum-title"))
        titleEl->SetInnerRML(escape(name));

    if (auto *topicEl = forumDocument->GetElementById("forum-topic")) {
        if (topic.empty()) {
            topicEl->SetProperty("display", "none");
            if (auto *divEl = forumDocument->GetElementById("forum-divider"))
                divEl->SetProperty("display", "none");
        } else {
            topicEl->SetProperty("display", "block");
            topicEl->SetInnerRML(escape(topic));
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
    if (tags.empty()) {
        tagsBar->SetInnerRML("");
        return;
    }

    std::string rml;
    const bool allActive = !selectedTagId.isValid();
    rml += std::format("<div class=\"forum-tag-chip{}\" data-action=\"filter-all\">All</div>",
                       allActive ? " active" : "");

    for (const auto &tag : tags) {
        if (!tag.id.hasValue() || !tag.name.hasValue())
            continue;

        const Core::Snowflake tagId = tag.id.get();
        const bool isActive = (selectedTagId == tagId);

        std::string iconPart;
        if (tag.emojiId.hasValue() && tag.emojiId.get().isValid()) {
            auto emojiUrl = Discord::Cdn::emoji(tag.emojiId.get(), 20);
            iconPart = std::format("<img class=\"tag-emoji\" src=\"{}\"/>",
                                   escape(shell.imageSource(emojiUrl, {14, 14})));
        } else if (tag.emojiName.hasValue() && !tag.emojiName->empty()) {
            iconPart = std::format("<span class=\"tag-emoji\">{}</span>", escape(tag.emojiName.get()));
        }

        rml += std::format("<div class=\"forum-tag-chip{}\" data-action=\"filter-tag\" data-tag-id=\"{}\">{}{}</div>",
                           isActive ? " active" : "", tagId.toString(), iconPart, escape(tag.name.get()));
    }

    tagsBar->SetInnerRML(rml);
}

std::string ForumController::thumbnailUrlOf(const Discord::Message *msg)
{
    if (!msg)
        return {};

    if (msg->attachments.hasValue()) {
        for (const auto &att : msg->attachments.get()) {
            if (!att.isImage())
                continue;
            if (att.proxyUrl.hasValue() && !att.proxyUrl.get().empty())
                return att.proxyUrl.get();
            if (att.url.hasValue() && !att.url.get().empty())
                return att.url.get();
        }
    }

    if (msg->embeds.hasValue()) {
        for (const auto &embed : msg->embeds.get()) {
            if (embed.image.hasValue()) {
                const auto &img = embed.image.get();
                if (img.proxyUrl.hasValue() && !img.proxyUrl.get().empty())
                    return img.proxyUrl.get();
                if (img.url.hasValue() && !img.url.get().empty())
                    return img.url.get();
            }
            if (embed.thumbnail.hasValue()) {
                const auto &thumb = embed.thumbnail.get();
                if (thumb.proxyUrl.hasValue() && !thumb.proxyUrl.get().empty())
                    return thumb.proxyUrl.get();
                if (thumb.url.hasValue() && !thumb.url.get().empty())
                    return thumb.url.get();
            }
        }
    }

    return {};
}

std::string ForumController::buildPostCardHtml(const Discord::Channel &post)
{
    const Core::Snowflake threadId = post.id.get();
    auto *inst = shell.instanceFor(currentAccountId);
    if (!inst || !inst->forums())
        return {};

    auto *mgr = inst->forums();
    mgr->ensureStarter(currentForumId, threadId);
    const Discord::Message *starter = mgr->firstMessagePtr(threadId);

    const auto readState = mgr->postReadState(threadId);
    const std::string title = post.name.valueOr("Untitled");

    std::string tagsHtml;
    if (post.appliedTags.hasValue() && !post.appliedTags->empty()) {
        const auto available = mgr->availableTags(currentForumId);
        std::string pills;
        for (const auto &tId : post.appliedTags.get()) {
            const auto it = std::find_if(available.begin(), available.end(), [tId](const auto &tag) {
                return tag.id.hasValue() && tag.id.get() == tId;
            });
            if (it == available.end() || !it->name.hasValue())
                continue;

            std::string iconPart;
            if (it->emojiId.hasValue() && it->emojiId.get().isValid()) {
                auto emojiUrl = Discord::Cdn::emoji(it->emojiId.get(), 20);
                iconPart = std::format("<img class=\"tag-emoji\" src=\"{}\"/>",
                                       escape(shell.imageSource(emojiUrl, {12, 12})));
            } else if (it->emojiName.hasValue() && !it->emojiName->empty()) {
                iconPart = std::format("<span class=\"tag-emoji\">{}</span>", escape(it->emojiName.get()));
            }
            pills += std::format("<div class=\"forum-card-tag-pill\">{}{}</div>", iconPart, escape(it->name.get()));
        }
        if (!pills.empty())
            tagsHtml = std::format("<div class=\"forum-card-tags-row\">{}</div>", pills);
    }

    std::string snippet;
    if (starter && starter->content.hasValue() && !starter->content->empty()) {
        const auto chars = Platform::codepoints(starter->content.get());
        std::vector<char32_t> simplified;
        bool space = false;
        for (char32_t ch : chars) {
            const bool whitespace = ch == U' ' || (ch >= U'\t' && ch <= U'\r') || ch == 0x85 ||
                                    ch == 0xa0 || ch == 0x1680 || (ch >= 0x2000 && ch <= 0x200a) ||
                                    ch == 0x2028 || ch == 0x2029 || ch == 0x202f || ch == 0x205f || ch == 0x3000;
            if (whitespace) {
                space = !simplified.empty();
            } else {
                if (space)
                    simplified.push_back(U' ');
                simplified.push_back(ch);
                space = false;
            }
        }
        if (simplified.size() > 140) {
            simplified.resize(137);
            simplified.push_back(U'…');
        }
        snippet = Platform::fromCodepoints(simplified);
    }

    std::string authorName = "User";
    std::string avatarSrc;
    Platform::Color authorColor;

    if (starter && starter->author.hasValue()) {
        const auto &author = starter->author.get();
        if (author.id.hasValue()) {
            const Core::Snowflake authorId = author.id.get();
            authorColor = shell.roleColor(authorId, currentGuildId);
            auto avatarUrl = Discord::Cdn::userAvatar(authorId, author.avatar.valueOr(""), 32);
            avatarSrc = shell.imageSource(avatarUrl, {18, 18});
        }
        if (author.globalName.hasValue() && !author.globalName->empty())
            authorName = author.globalName.get();
        else if (author.username.hasValue())
            authorName = author.username.get();
    }

    std::string authorStyle;
    if (authorColor.valid)
        authorStyle = std::format(" style=\"color: {};\"", authorColor.css());

    std::string avatarHtml;
    if (!avatarSrc.empty())
        avatarHtml = std::format("<img class=\"forum-card-avatar\" src=\"{}\"/>", escape(avatarSrc));

    const auto dt = post.effectiveLastMessageId().toDateTime();
    const std::string relTime = Core::TimeUtils::relativeTime(dt);
    const int replyCount = post.messageCount.valueOr(0);
    const auto thumbUrl = thumbnailUrlOf(starter);
    std::string thumbHtml;
    if (!thumbUrl.empty()) {
        const auto imgSrc = shell.imageSource(thumbUrl, {80, 80});
        thumbHtml = std::format("<div class=\"forum-card-thumbnail-container\">"
                                "<img class=\"forum-card-thumbnail\" src=\"{}\"/>"
                                "</div>", escape(imgSrc));
    }

    const std::string unreadDot = readState.unread ? "<div class=\"forum-card-unread-dot\"></div>" : "";
    return std::format(
        "<div class=\"forum-card\" data-action=\"open-post\" data-thread-id=\"{}\">"
        "  <div class=\"forum-card-left\">"
        "    <div class=\"forum-card-top-row\">"
        "      {}"
        "      <span class=\"forum-card-title{}\">{}</span>"
        "    </div>"
        "    {}"
        "    <div class=\"forum-card-snippet\">{}</div>"
        "    <div class=\"forum-card-footer\">"
        "      <div class=\"forum-card-meta-left\">"
        "        {}"
        "        <span class=\"forum-card-author\"{}>{}</span>"
        "        <span class=\"forum-card-time\">{}</span>"
        "      </div>"
        "      <div class=\"forum-card-meta-right\">"
        "        <div class=\"forum-card-replies\">"
        "          <img src=\"icon:message-circle\"/>"
        "          <span>{}</span>"
        "        </div>"
        "      </div>"
        "    </div>"
        "  </div>"
        "  {}"
        "</div>",
        threadId.toString(), unreadDot, readState.unread ? " unread" : "", escape(title),
        tagsHtml, escape(snippet), avatarHtml, authorStyle, escape(authorName), escape(relTime),
        replyCount, thumbHtml);
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

    std::string rml;
    int visibleCount = 0;

    for (const auto &post : posts) {
        if (selectedTagId.isValid()) {
            if (!post.appliedTags.hasValue() || std::find(post.appliedTags->begin(), post.appliedTags->end(), selectedTagId) == post.appliedTags->end())
                continue;
        }
        rml += buildPostCardHtml(post);
        ++visibleCount;
    }

    postsList->SetInnerRML(rml);

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
