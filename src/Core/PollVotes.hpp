#pragma once

#include "Core/Snowflake.hpp"

namespace Styx::Discord { struct Message; struct MessagePollVote; }

namespace Styx::Core {

// Applies one Gateway vote event to a known poll result. Message/channel and answer
// IDs must match. Unknown results remain unknown, and finalized tallies stay final.
// Self-vote transitions are idempotent; other voters' events adjust the count once
// per dispatch. Counts never go below zero or overflow. Preserve unrecognized JSON
// fields and keep Message::poll and pollJson paired. Returns true only on a change.
[[nodiscard]] bool applyPollVote(Discord::Message &message, const Discord::MessagePollVote &event,
                                  bool added, Snowflake selfId);

} // namespace Styx::Core
