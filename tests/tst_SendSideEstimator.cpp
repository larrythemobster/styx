// The estimate of what the way to the server carries, made from the server's reports of when
// each packet reached it. The way is one of the test's own: a queue that lets so many bits a
// second through, with the server's reports written by the same code this client reports to
// the server with.

#include <catch2/catch_test_macros.hpp>

#include "Discord/Voice/ProtocolEndian.hpp"
#include "Discord/Voice/Rtcp.hpp"
#include "Discord/Voice/SendSideEstimator.hpp"
#include "Discord/Voice/TransportFeedback.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

using namespace Styx::Discord::Voice;

namespace {

constexpr int PACKET_BYTES = 1200;
constexpr int64_t REPORT_EVERY_US = 100'000;
constexpr uint32_t SERVER_SSRC = 0xAABBCCDD;
constexpr uint32_t VIDEO_SSRC = 0x11223344;

// A queue that lets `capacity` bits a second through, and takes `delayUs` on top.
struct Way
{
    double capacity = 0;
    int64_t delayUs = 20'000;
    int64_t freeAtUs = 0;

    int64_t arrivalOf(int64_t sentUs, int bytes)
    {
        const int64_t startedUs = std::max(sentUs, freeAtUs);
        freeAtUs = startedUs + static_cast<int64_t>(bytes * 8e6 / capacity);
        return freeAtUs + delayUs;
    }
};

// A sender of evenly spaced packets, the way between it and the server, and the server's
// reports every tenth of a second.
class Call
{
public:
    explicit Call(double capacity, uint16_t firstSequence = 1)
    {
        way.capacity = capacity;
        estimator.restart(firstSequence);
    }

    // Sends for `seconds` at what `rate` gives each time it is asked, in bits per second.
    void run(double seconds, const std::function<double()> &rate)
    {
        const int64_t untilUs = nowUs + static_cast<int64_t>(seconds * 1e6);
        while (nowUs < untilUs) {
            if (nowUs >= nextPacketUs) {
                const uint16_t number = estimator.numberPacket(nowUs, PACKET_BYTES);
                onTheWay.push_back({ way.arrivalOf(nowUs, PACKET_BYTES), number });
                nextPacketUs = nowUs + static_cast<int64_t>(PACKET_BYTES * 8e6 / rate());
            }
            if (nowUs >= nextReportUs) {
                report();
                nextReportUs = nowUs + REPORT_EVERY_US;
            }
            nowUs += 500;
        }
    }

    void run(double seconds, double rate)
    {
        run(seconds, [rate] { return rate; });
    }

    SendSideEstimator estimator;
    Way way;
    uint64_t changes = 0;

private:
    struct OnTheWay
    {
        int64_t arrivalUs = 0;
        uint16_t number = 0;
    };

    void report()
    {
        while (!onTheWay.empty() && onTheWay.front().arrivalUs <= nowUs) {
            server.onPacket(onTheWay.front().number, onTheWay.front().arrivalUs);
            onTheWay.pop_front();
        }
        while (server.hasPending()) {
            std::vector<uint8_t> packet(1400);
            const int size = server.write(packet, SERVER_SSRC, VIDEO_SSRC);
            if (size <= 0)
                break;
            packet.resize(static_cast<size_t>(size));
            for (const ReceivedRtcp::TransportReport &read : parseRtcp(packet).transportReports)
                changes += estimator.onReport(read, nowUs) ? 1 : 0;
        }
    }

    TransportFeedback server;
    std::deque<OnTheWay> onTheWay;
    int64_t nowUs = 1'000'000;
    int64_t nextPacketUs = 0;
    int64_t nextReportUs = 0;
};

} // namespace

TEST_CASE("A report is read as it was written", "[estimator]")
{
    // Packets 10 to 15 with 12 missing, one of them arriving before the one numbered ahead
    // of it, and one a long while after the rest.
    TransportFeedback server;
    server.onPacket(10, 5'000'000);
    server.onPacket(11, 5'001'250);
    server.onPacket(13, 5'003'000);
    server.onPacket(14, 5'002'500);
    server.onPacket(15, 5'090'000);

    std::vector<uint8_t> packet(200);
    const int size = server.write(packet, SERVER_SSRC, VIDEO_SSRC);
    REQUIRE(size > 0);
    packet.resize(static_cast<size_t>(size));

    const ReceivedRtcp received = parseRtcp(packet);
    REQUIRE(received.transportReports.size() == 1);
    const ReceivedRtcp::TransportReport &report = received.transportReports.front();
    REQUIRE(report.mediaSsrc == VIDEO_SSRC);
    REQUIRE(report.baseSequence == uint16_t(10));
    REQUIRE(report.referenceTime == 5'000'000 / 64'000);
    REQUIRE(report.packets.size() == 6);

    // In quarter milliseconds from the reference time, which is 8 ms before the first.
    const std::vector<std::pair<bool, int>> expected = { { true, 32 }, { true, 5 }, { false, 0 }, { true, 7 }, { true, -2 }, { true, 350 } };
    for (size_t i = 0; i < expected.size(); i++) {
        REQUIRE(report.packets[i].received == expected[i].first);
        REQUIRE(report.packets[i].delta == expected[i].second);
    }
}

TEST_CASE("A report cut short is not read", "[estimator]")
{
    TransportFeedback server;
    for (uint16_t number = 0; number < 40; number++)
        server.onPacket(number, 1'000'000 + number * 3000);
    std::vector<uint8_t> packet(400);
    const int size = server.write(packet, SERVER_SSRC, VIDEO_SSRC);
    REQUIRE(size > 0);
    packet.resize(static_cast<size_t>(size));
    REQUIRE(parseRtcp(packet).transportReports.size() == 1);

    // The length in the header shortened by a word at a time: nothing of it can be read
    // once deltas are missing, and nothing is read past the end.
    for (int words = 1; size - 4 * words >= 20; words++) {
        std::vector<uint8_t> shorter(packet.begin(), packet.end() - 4 * words);
        writeBigEndian(static_cast<uint16_t>(shorter.size() / 4 - 1), shorter.data() + 2);
        shorter[0] &= ~0x20;
        REQUIRE(parseRtcp(shorter).transportReports.empty());
    }
}

TEST_CASE("Nothing is known of a way that keeps up", "[estimator]")
{
    Call call(5'000'000);
    call.run(20, 2'000'000);

    REQUIRE(call.estimator.estimate() == 0);
    REQUIRE(!call.estimator.overusing());
    REQUIRE(call.estimator.reportsTaken() > 150);
    const int64_t acknowledged = call.estimator.acknowledged();
    INFO("acknowledged: " << acknowledged);
    REQUIRE(acknowledged > 1'800'000);
    REQUIRE(acknowledged < 2'200'000);
}

TEST_CASE("A way that falls behind is backed off from", "[estimator]")
{
    // Twice what the way carries: every packet waits longer than the one before.
    Call call(1'000'000);
    call.run(3, 2'000'000);

    const int64_t estimate = call.estimator.estimate();
    INFO("estimate: " << estimate);
    REQUIRE(estimate > 700'000);
    REQUIRE(estimate < 950'000);
    const int64_t acknowledged = call.estimator.acknowledged();
    INFO("acknowledged: " << acknowledged);
    REQUIRE(acknowledged > 900'000);
    REQUIRE(acknowledged < 1'100'000);
    REQUIRE(call.changes > 0);
}

TEST_CASE("It climbs back once the way keeps up", "[estimator]")
{
    Call call(1'000'000);
    call.run(3, 2'000'000);
    const int64_t backedOff = call.estimator.estimate();
    REQUIRE(backedOff > 0);

    // The way widens, and the sender goes by the estimate: it creeps while it is near
    // where the way gave out, then climbs by a share of itself, never far above what is
    // seen to arrive.
    call.way.capacity = 6'000'000;
    call.run(5, [&call] { return static_cast<double>(call.estimator.estimate()); });
    const int64_t soonAfter = call.estimator.estimate();
    INFO(soonAfter << " after " << backedOff);
    REQUIRE(soonAfter > backedOff);
    REQUIRE(soonAfter < backedOff * 1.6);

    call.run(40, [&call] { return static_cast<double>(call.estimator.estimate()); });
    const int64_t later = call.estimator.estimate();
    INFO(later << " after " << backedOff);
    REQUIRE(later > backedOff * 3);
    REQUIRE(later < 6'000'000);
    REQUIRE(later <= call.estimator.acknowledged() * 1.5 + 10'000 + 1);
}

TEST_CASE("The numbering runs through its wrap", "[estimator]")
{
    // 2 Mbps is some two hundred packets a second, so the numbering wraps within three.
    Call call(6'000'000, 65'000);
    call.run(4, 2'000'000);
    const int64_t acknowledged = call.estimator.acknowledged();
    INFO("acknowledged: " << acknowledged);
    REQUIRE(acknowledged > 1'800'000);
    REQUIRE(acknowledged < 2'200'000);

    // What is acknowledged from here on is all of packets numbered after the wrap.
    call.run(4, 3'000'000);
    const int64_t afterTheWrap = call.estimator.acknowledged();
    INFO("afterTheWrap: " << afterTheWrap);
    REQUIRE(afterTheWrap > 2'700'000);
    REQUIRE(afterTheWrap < 3'300'000);
    REQUIRE(call.estimator.estimate() == 0);
}

TEST_CASE("A report on packets not sent is passed over", "[estimator]")
{
    ReceivedRtcp::TransportReport report;
    report.baseSequence = 500;
    report.packets.assign(20, { true, 4 });

    SendSideEstimator estimator;
    estimator.restart(1);
    REQUIRE(!estimator.onReport(report, 1'000'000));
    REQUIRE(estimator.reportsTaken() == 0);

    for (int i = 0; i < 10; i++)
        estimator.numberPacket(1'000'000 + i * 1000, PACKET_BYTES);
    REQUIRE(!estimator.onReport(report, 2'000'000));
    REQUIRE(estimator.acknowledged() == 0);
    REQUIRE(estimator.estimate() == 0);
}
