#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/Media/Player.hpp"

namespace Styx::Core::Media {

std::string attachmentKey(uint64_t attachmentId, MediaKind kind);
std::string embedKey(uint64_t messageId, int embedIndex);

class PlayerPool : public Platform::Trackable {
public:
    static constexpr int MaxActivePlayers = 3;
    PlayerPool() = default;
    ~PlayerPool() override;
    [[nodiscard]] Player *find(const std::string &key) const;
    Player *acquire(const std::string &key, const std::string &url, const ProxyConfig &proxy);
    void touch(const std::string &key);
    void release(const std::string &key);
    void clear();
    void setPinned(const std::string &key);

    Platform::Signal<std::string> frameReady;
    Platform::Signal<std::string> playerPositionChanged;
    Platform::Signal<std::string> playerStateChanged;
    Platform::Signal<std::string> playerReleased;
private:
    void enforceLimit();
    [[nodiscard]] std::string evictionCandidate() const;
    std::unordered_map<std::string, std::unique_ptr<Player>> players;
    std::vector<std::string> order;
    std::string pinnedKey;
};

} // namespace Styx::Core::Media
