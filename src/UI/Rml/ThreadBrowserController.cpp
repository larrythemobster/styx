#include "ThreadBrowserController.hpp"

#include "Shell.hpp"
#include "RmlRuntime.hpp"
#include "RmlView.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/TimeUtils.hpp"

#include "Platform/Native.hpp"
#include <format>
#include <charconv>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

namespace Styx {
namespace UI {
namespace Rml {

ThreadBrowserController::ThreadBrowserController(Shell &shell)
    : Controller(shell, nullptr)
{
}

ThreadBrowserController::~ThreadBrowserController()
{
    if (threadsDocument) {
        threadsDocument->Close();
        threadsDocument = nullptr;
    }
}

void ThreadBrowserController::ensureDocument()
{
    if (threadsDocument)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    threadsDocument = view->context()->LoadDocument("ui/threads.rml");
    if (!threadsDocument)
        return;

    regionRoot = threadsDocument;
    setupEventListeners();
}

void ThreadBrowserController::open(Core::ClientInstance *instance, Core::Snowflake channelId, const std::string &channelName)
{
    ensureDocument();
    if (!threadsDocument)
        return;

    clientInstance = instance;
    currentChannelId = channelId;
    currentChannelName = channelName;
    currentTab = Tab::Active;
    ++loadGeneration;

    activeThreads.clear();
    activeLoading = false;
    activeLoaded = false;
    activeError.clear();

    archivedThreads.clear();
    archivedLoading = false;
    archivedLoaded = false;
    archivedHasMore = false;
    archivedOffset = 0;
    archivedError.clear();

    visible = true;
    threadsDocument->Show(::Rml::ModalFlag::None);

    if (auto *nameEl = threadsDocument->GetElementById("threads-channel-name")) {
        std::string display = channelName.empty() ? std::string{} : "#" + channelName;
        nameEl->SetInnerRML(Platform::htmlEscape(display));
    }

    render();
    loadActive();
    loadArchived();
    requestRender();
}

void ThreadBrowserController::close()
{
    if (!visible)
        return;

    visible = false;
    ++loadGeneration;

    if (threadsDocument)
        threadsDocument->Hide();

    requestRender();
}

void ThreadBrowserController::setupEventListeners()
{
    if (!threadsDocument)
        return;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        auto *actionEl = closest(target, "data-action");
        if (!actionEl)
            return;

        const std::string action = attribute(actionEl, "data-action");

        if (action == "close") {
            close();
        } else if (action == "tab-active") {
            if (currentTab != Tab::Active) {
                currentTab = Tab::Active;
                render();
            }
        } else if (action == "tab-archived") {
            if (currentTab != Tab::Archived) {
                currentTab = Tab::Archived;
                render();
                if (!archivedLoaded && !archivedLoading)
                    loadArchived();
            }
        } else if (action == "load-more-archived") {
            if (!archivedLoading && archivedHasMore)
                loadArchived();
        } else if (action == "open-thread") {
            const auto idText = attribute(actionEl, "data-thread-id");
            std::uint64_t idVal = 0;
            std::from_chars(idText.data(), idText.data() + idText.size(), idVal);
            if (idVal != 0) {
                Core::Snowflake threadId(idVal);
                threadActivated.publish(threadId);
                close();
            }
        }
    });
}

void ThreadBrowserController::loadActive()
{
    if (!clientInstance || !currentChannelId.isValid())
        return;

    activeLoading = true;
    activeError.clear();
    render();

    const int gen = loadGeneration;
    clientInstance->fetchThreadList(
        currentChannelId,
        false,
        0,
        [this, guard = lifetime(), gen](const Core::Result<Core::ClientInstance::ThreadListPage> &res) {
            if (guard.expired() || gen != loadGeneration)
                return;
            activeLoading = false;
            activeLoaded = true;
            if (!res.success()) {
                activeError = "Failed to load active threads";
            } else {
                activeThreads = res.value->threads;
            }
            render();
            requestRender();
        });
}

void ThreadBrowserController::loadArchived()
{
    if (!clientInstance || !currentChannelId.isValid())
        return;

    archivedLoading = true;
    archivedError.clear();
    render();

    const int gen = loadGeneration;
    const int offset = archivedOffset;
    clientInstance->fetchThreadList(
        currentChannelId,
        true,
        offset,
        [this, guard = lifetime(), gen](const Core::Result<Core::ClientInstance::ThreadListPage> &res) {
            if (guard.expired() || gen != loadGeneration)
                return;
            archivedLoading = false;
            archivedLoaded = true;
            if (!res.success()) {
                archivedError = "Failed to load archived threads";
                archivedHasMore = false;
            } else {
                archivedThreads.insert(archivedThreads.end(), res.value->threads.begin(), res.value->threads.end());
                archivedOffset += static_cast<int>(res.value->threads.size());
                archivedHasMore = res.value->hasMore;
            }
            render();
            requestRender();
        });
}

void ThreadBrowserController::render()
{
    if (!threadsDocument)
        return;

    auto *tabActive = threadsDocument->GetElementById("tab-active");
    auto *tabArchived = threadsDocument->GetElementById("tab-archived");

    if (tabActive && tabArchived) {
        tabActive->SetClass("active", currentTab == Tab::Active);
        tabArchived->SetClass("active", currentTab == Tab::Archived);
    }

    auto *loadingEl = threadsDocument->GetElementById("threads-loading");
    auto *emptyEl = threadsDocument->GetElementById("threads-empty");
    auto *itemsEl = threadsDocument->GetElementById("threads-items");
    auto *loadMoreEl = threadsDocument->GetElementById("threads-load-more");

    if (!itemsEl)
        return;

    const bool isAct = (currentTab == Tab::Active);
    const bool isLoading = isAct ? activeLoading : archivedLoading;
    const bool isLoaded = isAct ? activeLoaded : archivedLoaded;
    const auto &threads = isAct ? activeThreads : archivedThreads;
    const std::string &error = isAct ? activeError : archivedError;

    if (loadingEl)
        loadingEl->SetProperty("display", isLoading && threads.empty() ? "block" : "none");

    if (emptyEl) {
        if (!error.empty()) {
            emptyEl->SetInnerRML(Platform::htmlEscape(error));
            emptyEl->SetProperty("display", "block");
        } else if (isLoaded && threads.empty()) {
            emptyEl->SetInnerRML(isAct ? "No active threads" : "No archived threads");
            emptyEl->SetProperty("display", "block");
        } else {
            emptyEl->SetProperty("display", "none");
        }
    }

    std::string rml;
    for (const auto &thread : threads) {
        const Core::Snowflake threadId = thread.id.get();
        std::string name = thread.name.valueOr(threadId.toString());
        const bool isPrivate = thread.type.hasValue() && thread.type.get() == Discord::ChannelType::PRIVATE_THREAD;
        const std::string icon = isPrivate ? "icon:lock" : "icon:messages-square";

        std::string meta;
        if (thread.messageCount.hasValue()) {
            meta += std::format("{} messages", thread.messageCount.get());
        }
        if (thread.lastMessageId.hasValue() && thread.lastMessageId->isValid()) {
            std::string rel = Core::TimeUtils::relativeTime(thread.lastMessageId->toDateTime());
            if (!meta.empty())
                meta += " • ";
            meta += rel;
        }

        rml += std::format(
            "<div class=\"thread-item-row\" data-action=\"open-thread\" data-thread-id=\"{}\">"
            "  <img class=\"thread-item-icon\" src=\"{}\"/>"
            "  <div class=\"thread-item-info\">"
            "    <span class=\"thread-item-name\">{}</span>"
            "    <span class=\"thread-item-meta\">{}</span>"
            "  </div>"
            "</div>", threadId.toString(), icon, Platform::htmlEscape(name), Platform::htmlEscape(meta));
    }

    itemsEl->SetInnerRML(rml);

    if (loadMoreEl) {
        const bool showMore = (!isAct && archivedHasMore && !archivedLoading);
        loadMoreEl->SetProperty("display", showMore ? "flex" : "none");
    }
}

} // namespace Rml
} // namespace UI
} // namespace Styx
