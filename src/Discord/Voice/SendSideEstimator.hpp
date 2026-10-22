// Ported from ouwou upstream/video-streams (71b4907); original implementation retained.
#pragma once


#include <cstdint>
#include <deque>
#include <optional>

#include "Rtcp.hpp"

namespace Styx {
namespace Discord {
namespace Voice {

// What the way from this client to the server carries, judged from the server's reports of
// when each video packet reached it (transport-wide congestion control). Packets that arrive
// further and further apart than they left are queueing somewhere, which shows before any is
// lost. It numbers the connection's video packets, which is what those reports go by.
//
// This is the delay-based half of WebRTC's GoogCC, which the official client runs unchanged:
// inter_arrival_delta.cc, trendline_estimator.cc, bitrate_estimator.cc, aimd_rate_control.cc
// and link_capacity_estimator.cc, with their numbers. Two things are left out. Nothing is
// sent to probe the way, so it knows of no limit until the way has once been overused, and
// then finds its way back up only as fast as what is sent grows.
//
// It has no clock: the owner passes the time in, in microseconds from any origin.
class SendSideEstimator
{
public:
    // Forgets everything, and numbers the next packet `firstSequence`.
    void restart(uint16_t firstSequence);

    // Numbers a packet that is about to leave.
    uint16_t numberPacket(int64_t nowUs, int bytes);
    // The round trip the server's reception reports gave.
    void setRoundTrip(int64_t roundTripUs);
    // One of the server's reports. Returns whether the estimate changed.
    bool onReport(const ReceivedRtcp::TransportReport &report, int64_t nowUs);

    // Bits per second. Zero until the way has once been overused.
    [[nodiscard]] int64_t estimate() const { return rate.initialized ? static_cast<int64_t>(rate.current) : 0; }
    // What the server has lately acknowledged receiving, in bits per second. Zero until that
    // has been measured.
    [[nodiscard]] int64_t acknowledged() const { return throughput.estimateKbps < 0 ? 0 : static_cast<int64_t>(throughput.estimateKbps * 1000); }
    [[nodiscard]] uint64_t reportsTaken() const { return reports; }
    [[nodiscard]] bool overusing() const { return trend.usage == Usage::Overusing; }

    static constexpr int MAX_PACKETS_HELD = 20000;
    static constexpr int64_t HELD_FOR_US = 60'000'000;
    static constexpr int64_t SILENCE_BEFORE_RESTART_US = 2'000'000;

private:
    enum class Usage {
        Normal,
        Underusing,
        Overusing,
    };

    struct Sent
    {
        int64_t sentUs = 0;
        // Zero once the server has reported on it.
        int bytes = 0;
    };

    // What is acknowledged per second, measured over windows and smoothed by how far each
    // window is from the estimate.
    struct Throughput
    {
        void add(int64_t atMs, int bytes);
        [[nodiscard]] float closeWindow(int64_t atMs, int bytes, int windowMs);

        float estimateKbps = -1;
        float variance = 50;
        int64_t sum = 0;
        int64_t windowMs = 0;
        int64_t previousMs = -1;
    };

    // Packets sent within a few milliseconds of each other are taken as one group, and each
    // group's spacing from the one before is compared with the spacing they arrived at.
    struct Grouping
    {
        struct Group
        {
            int64_t firstSendUs = -1;
            int64_t sendUs = -1;
            int64_t firstArrivalUs = -1;
            int64_t completeUs = -1;
            int64_t lastSystemUs = -1;
            [[nodiscard]] bool isEmpty() const { return completeUs < 0; }
        };

        // True when a group was completed, with the spacings in the two outputs.
        bool add(int64_t sendUs, int64_t arrivalUs, int64_t systemUs, int64_t *sendDeltaUs, int64_t *arrivalDeltaUs);
        [[nodiscard]] bool belongsToBurst(int64_t arrivalUs, int64_t sendUs) const;

        Group current;
        Group previous;
        int reorderedInARow = 0;
    };

    // The slope of the delay over the last groups, held against a threshold that follows it.
    struct Trend
    {
        struct Point
        {
            double arrivalMs = 0;
            double smoothedDelayMs = 0;
        };

        void add(double arrivalDeltaMs, double sendDeltaMs, double arrivalMs);
        void detect(double slope, double sendDeltaMs, double nowMs);
        void adaptThreshold(double modifiedTrend, double nowMs);

        std::deque<Point> window;
        double firstArrivalMs = -1;
        double accumulatedDelayMs = 0;
        double smoothedDelayMs = 0;
        int deltas = 0;
        double threshold = 12.5;
        double thresholdAdaptedMs = -1;
        double previousSlope = 0;
        double overusingForMs = -1;
        int overuses = 0;
        Usage usage = Usage::Normal;
    };

    // Where the way has been seen to give out, and how sure that is.
    struct Capacity
    {
        void onOveruse(double bitsPerSecond);
        [[nodiscard]] double upperBound() const;
        [[nodiscard]] double lowerBound() const;
        [[nodiscard]] double deviationKbps() const;

        std::optional<double> estimateKbps;
        double deviation = 0.4;
    };

    // Additive increase, multiplicative decrease.
    struct Rate
    {
        enum class State {
            Hold,
            Increase,
            Decrease,
        };

        [[nodiscard]] bool timeToReduceFurther(int64_t nowUs, double acknowledged) const;
        // Returns whether the bitrate changed.
        bool update(Usage usage, std::optional<double> acknowledged, int64_t nowUs);
        [[nodiscard]] double nearMaxIncreasePerSecond() const;

        State state = State::Hold;
        double current = 30'000'000;
        double latestAcknowledged = 30'000'000;
        bool initialized = false;
        int64_t changedUs = -1;
        int64_t roundTripUs = 200'000;
        Capacity capacity;
    };

    void forgetDelays();

    std::deque<Sent> sent;
    // The number of the first packet in `sent`, and of the next to leave, neither wrapped.
    int64_t firstHeld = 0;
    int64_t nextNumber = 0;

    bool haveReference = false;
    int32_t lastReference = 0;
    int64_t referenceOffsetUs = 0;
    int64_t lastArrivalSeenUs = -1;
    uint64_t reports = 0;

    Throughput throughput;
    Grouping grouping;
    Trend trend;
    Rate rate;
};

} // namespace Voice
} // namespace Discord
} // namespace Styx
