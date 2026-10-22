#pragma once

#include <cstdint>
#include <optional>

namespace Styx::Core::Video {

// Chooses the bitrate a video is encoded at, between its limits, from what the server says is
// being lost on the way. Ported from ouwou/Acheron video-streams.
class SendRateController {
public:
    void restart(int minBitrate, int maxBitrate);
    bool setLimits(int minBitrate, int maxBitrate);
    bool report(double lossFraction, std::int64_t nowMs);
    bool setReceiverLimit(std::int64_t bitsPerSecond);

    [[nodiscard]] int bitrate() const;

    static constexpr double HIGH_LOSS = 0.10;
    static constexpr double LOW_LOSS = 0.02;
    static constexpr double DECREASE = 0.7;
    static constexpr double INCREASE = 1.15;
    static constexpr std::int64_t DECREASE_INTERVAL_MS = 2000;
    static constexpr std::int64_t CLEAN_BEFORE_INCREASE_MS = 10000;

private:
    void moveTo(double wanted);

    int floor = 0;
    int ceiling = 0;
    int current = 0;
    std::optional<std::int64_t> receiverLimit;
    std::optional<std::int64_t> loweredAtMs;
    std::optional<std::int64_t> cleanSinceMs;
};

} // namespace Styx::Core::Video
