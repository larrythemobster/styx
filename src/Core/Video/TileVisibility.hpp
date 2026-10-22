#pragma once

#include <cstdint>
#include <map>

namespace Styx::Core {

// Decides whose video remains worth receiving after their tile leaves view. Ported from
// ouwou/Acheron video-streams: recent hidden tiles have a short grace period.
class TileVisibility {
public:
    void setVisible(std::uint64_t userId, bool visible, std::int64_t nowMs);
    void remove(std::uint64_t userId);
    void clear();
    [[nodiscard]] bool wanted(std::uint64_t userId, std::int64_t nowMs) const;
    [[nodiscard]] bool hasPendingExpiry(std::int64_t nowMs) const;
    static constexpr int HIDDEN_GRACE_MS = 30000;
    static constexpr int HIDDEN_KEPT = 3;
private:
    struct Tile { bool visible = false; std::int64_t hiddenSinceMs = 0; };
    std::map<std::uint64_t, Tile> tiles;
};

} // namespace Styx::Core
