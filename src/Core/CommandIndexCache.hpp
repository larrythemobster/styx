#pragma once

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/ApplicationCommands.hpp"
#include "Platform/Signal.hpp"
#include "Platform/Trackable.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace Styx::Core {

// Application command indexes for the places commands come from: a server, a DM or
// group DM, and the apps installed to the user. Each is fetched once per Gateway session;
// callers asking for a scope that is already on its way share that request.
class CommandIndexCache : public Platform::Trackable
{
public:
    enum class ScopeKind { Guild, Channel, User };
    struct Scope
    {
        ScopeKind kind = ScopeKind::User;
        Snowflake id; // the guild or channel; unused for User
        auto operator<=>(const Scope &) const = default;
    };

    using FetchCallback = std::function<void(Result<Discord::ApplicationCommandIndex>)>;
    using Fetch = std::function<void(Scope, FetchCallback)>;

    explicit CommandIndexCache(Fetch fetch);

    // The cached index, or null while it isn't available; a null return starts a fetch
    // unless one for the scope is already in flight. A failed fetch caches nothing, so
    // the next request tries again.
    const Discord::ApplicationCommandIndex *request(Scope scope);

    // Forgets every index, for a new session. Fetches still in flight are ignored when
    // they finish.
    void clear();

    Platform::Signal<Scope> indexReady;

private:
    Fetch fetch;
    std::map<Scope, Discord::ApplicationCommandIndex> indexes;
    std::set<Scope> inFlight;
    std::uint64_t generation = 0;
};

} // namespace Styx::Core
