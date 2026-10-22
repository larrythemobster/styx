#include "Core/Media/DecodeDemand.hpp"

#include <algorithm>

namespace Styx::Core::Media {
bool DecodeDemand::observe(int backlogMs, std::uint64_t queuesDiscarded, std::int64_t nowMs)
{
    const bool discardedSinceLast = queuesDiscarded != discardsSeen;
    discardsSeen = queuesDiscarded;
    const bool behind = backlogMs >= BEHIND_MS || discardedSinceLast;
    if (!behind) {
        behindSinceMs.reset();
        if (!keepingUpSinceMs) keepingUpSinceMs = nowMs;
        if (halvings == 0 || nowMs - *keepingUpSinceMs < raiseAfterMs) return false;
        --halvings; raisedAtMs = nowMs; keepingUpSinceMs = nowMs; return true;
    }
    keepingUpSinceMs.reset();
    if (!behindSinceMs) behindSinceMs = nowMs;
    const bool senderHasHadTime = !loweredAtMs || nowMs - *loweredAtMs >= SETTLE_MS;
    const bool behindLongEnough = discardedSinceLast || nowMs - *behindSinceMs >= BEHIND_FOR_MS;
    if (halvings == MAX_HALVINGS || !senderHasHadTime || !behindLongEnough) return false;
    lower(nowMs); return true;
}
void DecodeDemand::lower(std::int64_t nowMs)
{
    if (raisedAtMs && nowMs - *raisedAtMs < RAISE_ON_TRIAL_MS) raiseAfterMs = std::min(raiseAfterMs * 2, LONGEST_RAISE_AFTER_MS);
    ++halvings; loweredAtMs = nowMs; behindSinceMs = nowMs;
}
int DecodeDemand::pixelsToAskFor(int shownPixels, std::int64_t sentPixels) const
{
    const std::int64_t useful = sentPixels > 0 ? std::min<std::int64_t>(shownPixels, sentPixels) : shownPixels;
    return halvings == 0 ? shownPixels : static_cast<int>(useful >> halvings);
}
} // namespace Styx::Core::Media
