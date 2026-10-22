#pragma once

#include "Controller.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

#include <QList>
#include <QPointer>
#include <QString>

namespace Rml {
class ElementDocument;
}

namespace Styx {
namespace UI {
namespace Rml {

class ThreadBrowserController final : public Controller
{
    Q_OBJECT
public:
    explicit ThreadBrowserController(Shell &shell, QObject *parent = nullptr);
    ~ThreadBrowserController() override;

    void open(Core::ClientInstance *instance, Core::Snowflake channelId, const QString &channelName);
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

signals:
    void threadActivated(Core::Snowflake threadId);

private:
    enum class Tab {
        Active,
        Archived
    };

    void ensureDocument();
    void setupEventListeners();
    void render();
    void loadActive();
    void loadArchived();

    ::Rml::ElementDocument *threadsDocument = nullptr;
    bool visible = false;

    QPointer<Core::ClientInstance> clientInstance;
    Core::Snowflake currentChannelId;
    QString currentChannelName;

    Tab currentTab = Tab::Active;
    int loadGeneration = 0;

    QList<Discord::Channel> activeThreads;
    bool activeLoading = false;
    bool activeLoaded = false;
    QString activeError;

    QList<Discord::Channel> archivedThreads;
    bool archivedLoading = false;
    bool archivedLoaded = false;
    bool archivedHasMore = false;
    int archivedOffset = 0;
    QString archivedError;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
