#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "Platform/Signal.hpp"

#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"
#include "Snowflake.hpp"

namespace Styx {
namespace Core {

class RelationshipManager
{
public:
    RelationshipManager() = default;

    void loadFromReady(const std::vector<Discord::Relationship> &relationships);

    [[nodiscard]] std::optional<Discord::Relationship> getRelationship(Snowflake userId) const;

    [[nodiscard]] std::string getNickname(Snowflake userId) const;

    [[nodiscard]] bool isFriend(Snowflake userId) const;
    [[nodiscard]] bool isBlockedOrIgnored(Snowflake userId) const;
    // Incoming friend requests, leaving out the ones Discord filed as spam, as the official
    // client's Friends badge counts them.
    [[nodiscard]] int incomingRequestCount() const;
    [[nodiscard]] const std::unordered_map<Snowflake, Discord::Relationship> &all() const { return store; }

    void onRelationshipAdded(const Discord::Relationship &rel);
    void onRelationshipUpdated(const Discord::RelationshipPartial &rel);
    void onRelationshipRemoved(const Discord::RelationshipPartial &rel);

    Platform::Signal<Snowflake> relationshipChanged;
    Platform::Signal<> relationshipsReset;

private:
    std::unordered_map<Snowflake, Discord::Relationship> store;
};

} // namespace Core
} // namespace Styx
