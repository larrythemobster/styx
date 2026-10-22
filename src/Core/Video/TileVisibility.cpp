#include "Core/Video/TileVisibility.hpp"

namespace Styx::Core {
void TileVisibility::setVisible(std::uint64_t userId, bool visible, std::int64_t nowMs)
{
    auto it = tiles.find(userId);
    if (it == tiles.end()) { tiles.emplace(userId, Tile{visible, nowMs}); return; }
    if (it->second.visible == visible) return;
    it->second.visible = visible;
    if (!visible) it->second.hiddenSinceMs = nowMs;
}
void TileVisibility::remove(std::uint64_t userId) { tiles.erase(userId); }
void TileVisibility::clear() { tiles.clear(); }
bool TileVisibility::wanted(std::uint64_t userId, std::int64_t nowMs) const
{
    const auto it = tiles.find(userId);
    if (it == tiles.end()) return false;
    if (it->second.visible) return true;
    if (nowMs - it->second.hiddenSinceMs >= HIDDEN_GRACE_MS) return false;
    int hiddenMoreRecently = 0;
    for (const auto &[otherId, other] : tiles) {
        if (otherId == userId || other.visible) continue;
        if (other.hiddenSinceMs > it->second.hiddenSinceMs || (other.hiddenSinceMs == it->second.hiddenSinceMs && otherId > userId)) ++hiddenMoreRecently;
    }
    return hiddenMoreRecently < HIDDEN_KEPT;
}
bool TileVisibility::hasPendingExpiry(std::int64_t nowMs) const
{
    for (const auto &[unused, tile] : tiles) if (!tile.visible && nowMs - tile.hiddenSinceMs < HIDDEN_GRACE_MS) return true;
    return false;
}
} // namespace Styx::Core
