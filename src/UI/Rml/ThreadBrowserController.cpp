#include "ThreadBrowserController.hpp"

#include "Shell.hpp"
#include "RmlRuntime.hpp"
#include "RmlView.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/TimeUtils.hpp"

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

ThreadBrowserController::ThreadBrowserController(Shell &shell, QObject *parent)
    : Controller(shell, nullptr, parent)
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

    threadsDocument = view->context()->LoadDocument(":/ui/threads.rml");
    if (!threadsDocument)
        return;

    regionRoot = threadsDocument;
    setupEventListeners();
}

void ThreadBrowserController::open(Core::ClientInstance *instance, Core::Snowflake channelId, const QString &channelName)
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
        QString display = channelName.isEmpty() ? QString() : QStringLiteral("#%1").arg(channelName);
        nameEl->SetInnerRML(escape(display).toStdString());
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

        const QString action = attribute(actionEl, "data-action");

        if (action == QStringLiteral("close")) {
            close();
        } else if (action == QStringLiteral("tab-active")) {
            if (currentTab != Tab::Active) {
                currentTab = Tab::Active;
                render();
            }
        } else if (action == QStringLiteral("tab-archived")) {
            if (currentTab != Tab::Archived) {
                currentTab = Tab::Archived;
                render();
                if (!archivedLoaded && !archivedLoading)
                    loadArchived();
            }
        } else if (action == QStringLiteral("load-more-archived")) {
            if (!archivedLoading && archivedHasMore)
                loadArchived();
        } else if (action == QStringLiteral("open-thread")) {
            const quint64 idVal = attribute(actionEl, "data-thread-id").toULongLong();
            if (idVal != 0) {
                Core::Snowflake threadId(idVal);
                emit threadActivated(threadId);
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
        [this, gen](const Core::Result<Core::ClientInstance::ThreadListPage> &res) {
            if (gen != loadGeneration)
                return;
            activeLoading = false;
            activeLoaded = true;
            if (!res.success()) {
                activeError = QStringLiteral("Failed to load active threads");
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
        [this, gen](const Core::Result<Core::ClientInstance::ThreadListPage> &res) {
            if (gen != loadGeneration)
                return;
            archivedLoading = false;
            archivedLoaded = true;
            if (!res.success()) {
                archivedError = QStringLiteral("Failed to load archived threads");
                archivedHasMore = false;
            } else {
                archivedThreads.append(res.value->threads);
                archivedOffset += res.value->threads.size();
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
    const QString &error = isAct ? activeError : archivedError;

    if (loadingEl)
        loadingEl->SetProperty("display", isLoading && threads.isEmpty() ? "block" : "none");

    if (emptyEl) {
        if (!error.isEmpty()) {
            emptyEl->SetInnerRML(escape(error).toStdString());
            emptyEl->SetProperty("display", "block");
        } else if (isLoaded && threads.isEmpty()) {
            emptyEl->SetInnerRML(isAct ? "No active threads" : "No archived threads");
            emptyEl->SetProperty("display", "block");
        } else {
            emptyEl->SetProperty("display", "none");
        }
    }

    QString rml;
    for (const auto &thread : threads) {
        const Core::Snowflake threadId = thread.id.get();
        QString name = thread.name.valueOr(QString::number(static_cast<quint64>(threadId)));
        const bool isPrivate = thread.type.hasValue() && thread.type.get() == Discord::ChannelType::PRIVATE_THREAD;
        const QString icon = isPrivate ? QStringLiteral("icon:lock") : QStringLiteral("icon:messages-square");

        QString meta;
        if (thread.messageCount.hasValue()) {
            meta += QStringLiteral("%1 messages").arg(thread.messageCount.get());
        }
        if (thread.lastMessageId.hasValue() && thread.lastMessageId->isValid()) {
            QString rel = Core::TimeUtils::relativeTime(thread.lastMessageId->toDateTime());
            if (!meta.isEmpty())
                meta += QStringLiteral(" • ");
            meta += rel;
        }

        rml += QStringLiteral(
            "<div class=\"thread-item-row\" data-action=\"open-thread\" data-thread-id=\"%1\">"
            "  <img class=\"thread-item-icon\" src=\"%2\"/>"
            "  <div class=\"thread-item-info\">"
            "    <span class=\"thread-item-name\">%3</span>"
            "    <span class=\"thread-item-meta\">%4</span>"
            "  </div>"
            "</div>")
            .arg(QString::number(static_cast<quint64>(threadId)),
                 icon,
                 escape(name),
                 escape(meta));
    }

    itemsEl->SetInnerRML(rml.toStdString());

    if (loadMoreEl) {
        const bool showMore = (!isAct && archivedHasMore && !archivedLoading);
        loadMoreEl->SetProperty("display", showMore ? "flex" : "none");
    }
}

} // namespace Rml
} // namespace UI
} // namespace Styx
