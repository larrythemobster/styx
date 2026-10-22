#pragma once

#include "Controller.hpp"
#include "Core/ForumManager.hpp"
#include "Core/Snowflake.hpp"

#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QUrl>

namespace Rml {
class ElementDocument;
}

namespace Styx {
namespace Core {
class ClientInstance;
class ForumManager;
}
namespace Discord {
struct Channel;
struct Message;
}
namespace UI {
namespace Rml {

class ForumController final : public Controller
{
    Q_OBJECT
public:
    explicit ForumController(Shell &shell, QObject *parent = nullptr);
    ~ForumController() override;

    void open(Core::Snowflake accountId, Core::Snowflake guildId, Core::Snowflake forumId);
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }
    [[nodiscard]] Core::Snowflake forumId() const { return currentForumId; }

    void syncFolderState();

private slots:
    void onPostsReset(Core::Snowflake forumId);
    void onPostsAppended(Core::Snowflake forumId, int startRow, int count);
    void onPostInserted(Core::Snowflake forumId, Core::Snowflake threadId, int row);
    void onPostUpdated(Core::Snowflake forumId, Core::Snowflake threadId);
    void onPostRemoved(Core::Snowflake forumId, Core::Snowflake threadId);
    void onLoadingChanged(Core::Snowflake forumId, bool loading);

private:
    void ensureDocument();
    void setupEventListeners();
    void updateHeader();
    void updateControls();
    void updateTags();
    void renderPosts();
    void updateLoadingState();

    QString buildPostCardHtml(const Discord::Channel &post);
    [[nodiscard]] static QUrl thumbnailUrlOf(const Discord::Message *msg);

    ::Rml::ElementDocument *forumDocument = nullptr;
    bool visible = false;

    Core::Snowflake currentAccountId;
    Core::Snowflake currentGuildId;
    Core::Snowflake currentForumId;
    Core::Snowflake selectedTagId;
    Core::ForumSortMode currentSortMode = Core::ForumSortMode::LATEST_ACTIVITY;

    QList<QMetaObject::Connection> managerConnections;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
