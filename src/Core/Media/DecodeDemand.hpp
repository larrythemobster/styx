#pragma once

#include <cstdint>
#include <optional>

namespace Styx::Core::Media {

// Scales sender pixel demand according to persistent decoder backlog. Ported from
// ouwou/Acheron video-streams.
class DecodeDemand {
public:
    bool observe(int backlogMs, std::uint64_t queuesDiscarded, std::int64_t nowMs);
    [[nodiscard]] double pixelShare() const { return 1.0 / (1 << halvings); }
    [[nodiscard]] int pixelsToAskFor(int shownPixels, std::int64_t sentPixels) const;
    static constexpr int BEHIND_MS = 250;
    static constexpr int BEHIND_FOR_MS = 2000;
    static constexpr int SETTLE_MS = 5000;
    static constexpr int MAX_HALVINGS = 2;
    static constexpr int FIRST_RAISE_AFTER_MS = 60'000;
    static constexpr int LONGEST_RAISE_AFTER_MS = 480'000;
    static constexpr int RAISE_ON_TRIAL_MS = 60'000;
private:
    void lower(std::int64_t nowMs);
    int halvings = 0;
    std::uint64_t discardsSeen = 0;
    std::optional<std::int64_t> behindSinceMs;
    std::optional<std::int64_t> keepingUpSinceMs;
    std::optional<std::int64_t> loweredAtMs;
    std::optional<std::int64_t> raisedAtMs;
    int raiseAfterMs = FIRST_RAISE_AFTER_MS;
};

} // namespace Styx::Core::Media
