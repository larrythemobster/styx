#pragma once

#include <QHash>
#include <QObject>

#include <optional>

#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"
#include "Snowflake.hpp"

namespace Styx {
namespace Core {

class RelationshipManager : public QObject
{
    Q_OBJECT
public:
    explicit RelationshipManager(QObject *parent = nullptr);

    void loadFromReady(const QList<Discord::Relationship> &relationships);

    [[nodiscard]] std::optional<Discord::Relationship> getRelationship(Snowflake userId) const;

    [[nodiscard]] QString getNickname(Snowflake userId) const;

    [[nodiscard]] bool isFriend(Snowflake userId) const;
    [[nodiscard]] bool isBlockedOrIgnored(Snowflake userId) const;
    [[nodiscard]] const QHash<Snowflake, Discord::Relationship> &all() const { return store; }

public slots:
    void onRelationshipAdded(const Discord::Relationship &rel);
    void onRelationshipUpdated(const Discord::RelationshipPartial &rel);
    void onRelationshipRemoved(const Discord::RelationshipPartial &rel);

signals:
    void relationshipChanged(Snowflake userId);
    void relationshipsReset();

private:
    QHash<Snowflake, Discord::Relationship> store;
};

} // namespace Core
} // namespace Styx
