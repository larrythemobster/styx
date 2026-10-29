#include "Core/Media/PlayerPool.hpp"
#include "Core/Logging.hpp"
#include <algorithm>

namespace Styx::Core::Media {

std::string attachmentKey(uint64_t id, MediaKind kind)
{
    return std::string(kind == MediaKind::Audio ? "audio:" : "video:") + std::to_string(id);
}
std::string embedKey(uint64_t id, int index)
{
    return "embed:" + std::to_string(id) + ":" + std::to_string(index);
}
PlayerPool::~PlayerPool() { clear(); }
Player *PlayerPool::find(const std::string &key) const
{
    auto it = players.find(key);
    return it == players.end() ? nullptr : it->second.get();
}
Player *PlayerPool::acquire(const std::string &key, const std::string &url, const ProxyConfig &proxy)
{
    if (!isSupported() || key.empty() || url.empty()) return nullptr;
    if (auto *existing = find(key)) { touch(key); return existing; }
    enforceLimit();
    auto owned = std::make_unique<Player>();
    auto *player = owned.get();
    players.emplace(key, std::move(owned));
    order.push_back(key);
    player->track(player->frameReady.connect([this, key] { frameReady.publish(key); }));
    player->track(player->positionChanged.connect([this, key](int64_t) { playerPositionChanged.publish(key); }));
    player->track(player->stateChanged.connect([this, key](Player::State) { playerStateChanged.publish(key); }));
    player->track(player->errorOccurred.connect([key](const std::string &message) {
        Core::log(Core::LogLevel::Warning, LogVideo) << "player" << key << "failed:" << message;
    }));
    player->open(url, proxy);
    return player;
}
void PlayerPool::touch(const std::string &key)
{
    if (!players.contains(key)) return;
    std::erase(order, key);
    order.push_back(key);
}
void PlayerPool::setPinned(const std::string &key) { pinnedKey = key; }
void PlayerPool::release(const std::string &key)
{
    auto it = players.find(key);
    if (it == players.end()) return;
    auto player = std::move(it->second);
    players.erase(it);
    std::erase(order, key);
    if (pinnedKey == key) pinnedKey.clear();
    player->close();
    playerReleased.publish(key);
}
void PlayerPool::clear()
{
    while (!players.empty()) {
        const auto key = players.begin()->first;
        release(key);
    }
}
std::string PlayerPool::evictionCandidate() const
{
    std::string fallback;
    for (const auto &key : order) {
        if (key == pinnedKey) continue;
        if (!find(key)->isPlaying()) return key;
        if (fallback.empty()) fallback = key;
    }
    return fallback;
}
void PlayerPool::enforceLimit()
{
    while (players.size() >= MaxActivePlayers) {
        const auto victim = evictionCandidate();
        if (victim.empty()) return;
        Core::log(Core::LogLevel::Debug, LogVideo) << "evicting player" << victim << "to stay under the decoder cap";
        release(victim);
    }
}
} // namespace Styx::Core::Media
