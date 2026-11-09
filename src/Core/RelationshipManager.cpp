#include "RelationshipManager.hpp"

#include "Logging.hpp"

namespace Styx {
namespace Core {

void RelationshipManager::loadFromReady(const std::vector<Discord::Relationship> &relationships)
{
    store.clear();
    store.reserve(relationships.size());
    for (const auto &r : relationships) {
        if (r.id.hasValue())
            store.insert_or_assign(r.id.get(), r);
    }
    log(LogLevel::Info, LogCore) << "Loaded" << store.size() << "relationships from READY";
    relationshipsReset.publish();
}

std::optional<Discord::Relationship> RelationshipManager::getRelationship(Snowflake userId) const
{
    auto it = store.find(userId);
    if (it == store.end())
        return std::nullopt;
    return it->second;
}

std::string RelationshipManager::getNickname(Snowflake userId) const
{
    auto it = store.find(userId);
    return it != store.end() ? it->second.nickname.valueOr() : std::string();
}

bool RelationshipManager::isFriend(Snowflake userId) const
{
    auto it = store.find(userId);
    if (it == store.end())
        return false;
    if (!it->second.type.hasValue())
        return false;
    return it->second.type.get() == Discord::RelationshipType::FRIEND;
}

int RelationshipManager::incomingRequestCount() const
{
    int count = 0;
    for (const auto &[id, rel] : store)
        if (rel.type.hasValue() && rel.type.get() == Discord::RelationshipType::INCOMING_REQUEST && !rel.isSpamRequest.valueOr(false))
            ++count;
    return count;
}

bool RelationshipManager::isBlockedOrIgnored(Snowflake userId) const
{
    auto it = store.find(userId);
    if (it == store.end())
        return false;
    const auto &rel = it->second;
    return (rel.type.hasValue() && rel.type.get() == Discord::RelationshipType::BLOCKED) ||
           (rel.userIgnored.hasValue() && rel.userIgnored.get());
}

void RelationshipManager::onRelationshipAdded(const Discord::Relationship &rel)
{
    if (!rel.id.hasValue())
        return;
    Snowflake id = rel.id.get();
    store.insert_or_assign(id, rel);
    relationshipChanged.publish(id);
}

void RelationshipManager::onRelationshipUpdated(const Discord::RelationshipPartial &rel)
{
    if (!rel.id.hasValue())
        return;
    Snowflake id = rel.id.get();
    auto it = store.find(id);
    if (it == store.end())
        return;
    it->second.type = rel.type.get();
    if (rel.nickname.hasValue())
        it->second.nickname = rel.nickname.get();
    else if (rel.nickname.isNull())
        it->second.nickname = nullptr;
    if (rel.since.hasValue())
        it->second.since = rel.since.get();
    if (rel.strangerRequest.hasValue())
        it->second.strangerRequest = rel.strangerRequest.get();
    it->second.userIgnored = rel.userIgnored.get();
    relationshipChanged.publish(id);
}

void RelationshipManager::onRelationshipRemoved(const Discord::RelationshipPartial &rel)
{
    if (!rel.id.hasValue())
        return;
    Snowflake id = rel.id.get();
    if (store.erase(id))
        relationshipChanged.publish(id);
}

} // namespace Core
} // namespace Styx
