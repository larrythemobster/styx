#include "Core/CommandIndexCache.hpp"

#include <utility>

namespace Styx::Core {

CommandIndexCache::CommandIndexCache(Fetch fetch)
    : fetch(std::move(fetch))
{
}

const Discord::ApplicationCommandIndex *CommandIndexCache::request(Scope scope)
{
    if (scope.kind == ScopeKind::User)
        scope.id = Snowflake::Invalid;

    const auto it = indexes.find(scope);
    if (it != indexes.end())
        return &it->second;

    if (inFlight.contains(scope))
        return nullptr;

    inFlight.insert(scope);
    Platform::Guard<CommandIndexCache> self(this);
    const auto capturedGeneration = generation;

    fetch(scope, [self, capturedGeneration, scope](Result<Discord::ApplicationCommandIndex> result) {
        if (!self || self->generation != capturedGeneration)
            return;

        self->inFlight.erase(scope);
        if (result.success()) {
            self->indexes.insert_or_assign(scope, std::move(*result.value));
            self->indexReady.publish(scope);
        }
    });

    // The fetch may have answered synchronously.
    const auto stored = indexes.find(scope);
    return stored != indexes.end() ? &stored->second : nullptr;
}

void CommandIndexCache::clear()
{
    indexes.clear();
    inFlight.clear();
    ++generation;
}

} // namespace Styx::Core
