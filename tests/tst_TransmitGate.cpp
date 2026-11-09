#include "Core/Audio/TransmitGate.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace Styx::Core::Audio;

namespace {

struct StepCounts {
    int none = 0;
    int started = 0;
    int stopped = 0;
};

StepCounts stepFrames(TransmitGate &gate, int count, bool voiceDetected)
{
    StepCounts counts;
    for (int i = 0; i < count; ++i) {
        switch (gate.step(voiceDetected)) {
        case TransmitGate::Change::None:
            counts.none++;
            break;
        case TransmitGate::Change::Started:
            counts.started++;
            break;
        case TransmitGate::Change::Stopped:
            counts.stopped++;
            break;
        }
    }
    return counts;
}

} // namespace

TEST_CASE("Voice activity hold frames and transitions", "[transmit-gate]")
{
    TransmitGate gate;
    REQUIRE(gate.mode() == TransmitGate::Mode::VoiceActivity);
    REQUIRE_FALSE(gate.transmitting());

    // Silent frames before any speech keep the gate closed
    REQUIRE(gate.step(false) == TransmitGate::Change::None);
    REQUIRE_FALSE(gate.transmitting());

    // A voice frame opens the gate and reports Started
    REQUIRE(gate.step(true) == TransmitGate::Change::Started);
    REQUIRE(gate.transmitting());

    // Silent frames stay open for exactly 25 frames, returning None
    const auto silentHold = stepFrames(gate, 25, false);
    REQUIRE(silentHold.none == 25);
    REQUIRE(silentHold.started == 0);
    REQUIRE(silentHold.stopped == 0);
    REQUIRE(gate.transmitting());

    // The 26th silent frame closes the gate and reports Stopped
    REQUIRE(gate.step(false) == TransmitGate::Change::Stopped);
    REQUIRE_FALSE(gate.transmitting());

    // Further silence produces no additional events
    REQUIRE(gate.step(false) == TransmitGate::Change::None);
    REQUIRE_FALSE(gate.transmitting());
}

TEST_CASE("Push-to-talk ignores voice and requires key held", "[transmit-gate]")
{
    TransmitGate gate;
    gate.setMode(TransmitGate::Mode::PushToTalk);
    REQUIRE(gate.mode() == TransmitGate::Mode::PushToTalk);

    // Voice frames with the key up never start transmission
    const auto voiceWithoutKey = stepFrames(gate, 10, true);
    REQUIRE(voiceWithoutKey.none == 10);
    REQUIRE(voiceWithoutKey.started == 0);
    REQUIRE(voiceWithoutKey.stopped == 0);
    REQUIRE_FALSE(gate.transmitting());

    // Key held starts transmission on next step even with voiceDetected=false
    gate.setKeyHeld(true);
    REQUIRE(gate.step(false) == TransmitGate::Change::Started);
    REQUIRE(gate.transmitting());

    // Key still held stays open without re-starting
    REQUIRE(gate.step(false) == TransmitGate::Change::None);
    REQUIRE(gate.transmitting());
}

TEST_CASE("Push-to-talk release delays", "[transmit-gate]")
{
    TransmitGate gate;
    gate.setMode(TransmitGate::Mode::PushToTalk);

    SECTION("Delay 0: letting go closes gate on very next step") {
        gate.setReleaseDelayMs(0);
        REQUIRE(gate.releaseDelayFrames() == 0);
        gate.setKeyHeld(true);
        REQUIRE(gate.step(false) == TransmitGate::Change::Started);

        gate.setKeyHeld(false);
        REQUIRE(gate.step(false) == TransmitGate::Change::Stopped);
        REQUIRE_FALSE(gate.transmitting());
    }

    SECTION("Delay 20 ms: one more open frame, then Stopped") {
        gate.setReleaseDelayMs(20);
        REQUIRE(gate.releaseDelayFrames() == 1);
        gate.setKeyHeld(true);
        REQUIRE(gate.step(false) == TransmitGate::Change::Started);

        gate.setKeyHeld(false);
        REQUIRE(gate.step(false) == TransmitGate::Change::None);
        REQUIRE(gate.transmitting());
        REQUIRE(gate.step(false) == TransmitGate::Change::Stopped);
        REQUIRE_FALSE(gate.transmitting());
    }

    SECTION("Delay 100 ms: 5 open frames, then Stopped") {
        gate.setReleaseDelayMs(100);
        REQUIRE(gate.releaseDelayFrames() == 5);
        gate.setKeyHeld(true);
        REQUIRE(gate.step(false) == TransmitGate::Change::Started);

        gate.setKeyHeld(false);
        const auto hold = stepFrames(gate, 5, false);
        REQUIRE(hold.none == 5);
        REQUIRE(hold.started == 0);
        REQUIRE(hold.stopped == 0);
        REQUIRE(gate.transmitting());

        REQUIRE(gate.step(false) == TransmitGate::Change::Stopped);
        REQUIRE_FALSE(gate.transmitting());
    }
}

TEST_CASE("Release delay rounding and clamping", "[transmit-gate]")
{
    TransmitGate gate;

    gate.setReleaseDelayMs(0);
    REQUIRE(gate.releaseDelayFrames() == 0);

    gate.setReleaseDelayMs(1);
    REQUIRE(gate.releaseDelayFrames() == 1);

    gate.setReleaseDelayMs(20);
    REQUIRE(gate.releaseDelayFrames() == 1);

    gate.setReleaseDelayMs(21);
    REQUIRE(gate.releaseDelayFrames() == 2);

    gate.setReleaseDelayMs(2000);
    REQUIRE(gate.releaseDelayFrames() == 100);

    gate.setReleaseDelayMs(5000);
    REQUIRE(gate.releaseDelayFrames() == 100);

    gate.setReleaseDelayMs(-5);
    REQUIRE(gate.releaseDelayFrames() == 0);
}

TEST_CASE("Pressing again during release delay keeps transmission open", "[transmit-gate]")
{
    TransmitGate gate;
    gate.setMode(TransmitGate::Mode::PushToTalk);
    gate.setReleaseDelayMs(100);

    gate.setKeyHeld(true);
    REQUIRE(gate.step(false) == TransmitGate::Change::Started);
    REQUIRE(gate.transmitting());

    // Release key and step partially through the 5-frame release delay
    gate.setKeyHeld(false);
    REQUIRE(gate.step(false) == TransmitGate::Change::None);
    REQUIRE(gate.transmitting());
    REQUIRE(gate.step(false) == TransmitGate::Change::None);
    REQUIRE(gate.transmitting());

    // Press again during delay: gate stays open, no Stopped and no second Started
    gate.setKeyHeld(true);
    REQUIRE(gate.step(false) == TransmitGate::Change::None);
    REQUIRE(gate.transmitting());

    const auto whileHeld = stepFrames(gate, 4, false);
    REQUIRE(whileHeld.none == 4);
    REQUIRE(whileHeld.started == 0);
    REQUIRE(whileHeld.stopped == 0);
    REQUIRE(gate.transmitting());

    // Release again and let it run out completely
    gate.setKeyHeld(false);
    const auto hold = stepFrames(gate, 5, false);
    REQUIRE(hold.none == 5);
    REQUIRE(hold.started == 0);
    REQUIRE(hold.stopped == 0);
    REQUIRE(gate.transmitting());

    REQUIRE(gate.step(false) == TransmitGate::Change::Stopped);
    REQUIRE_FALSE(gate.transmitting());
}

TEST_CASE("close while open and closed", "[transmit-gate]")
{
    TransmitGate gate;

    // Gate closed: close returns false
    REQUIRE_FALSE(gate.transmitting());
    REQUIRE_FALSE(gate.close());

    // Open gate
    REQUIRE(gate.step(true) == TransmitGate::Change::Started);
    REQUIRE(gate.transmitting());

    // Close while open returns true
    REQUIRE(gate.close());
    REQUIRE_FALSE(gate.transmitting());

    // Next step without input returns None, not Stopped
    REQUIRE(gate.step(false) == TransmitGate::Change::None);
    REQUIRE_FALSE(gate.transmitting());

    // Subsequent close returns false
    REQUIRE_FALSE(gate.close());
}

TEST_CASE("Switching mode to VoiceActivity clears a held key", "[transmit-gate]")
{
    TransmitGate gate;
    gate.setMode(TransmitGate::Mode::PushToTalk);
    gate.setKeyHeld(true);

    gate.setMode(TransmitGate::Mode::VoiceActivity);
    gate.setMode(TransmitGate::Mode::PushToTalk);

    // Stepping with no voice never returns Started
    const auto counts = stepFrames(gate, 10, false);
    REQUIRE(counts.none == 10);
    REQUIRE(counts.started == 0);
    REQUIRE(counts.stopped == 0);
    REQUIRE_FALSE(gate.transmitting());
}

TEST_CASE("Switching to push-to-talk drops the voice activity holdoff", "[transmit-gate]")
{
    TransmitGate gate;
    REQUIRE(gate.step(true) == TransmitGate::Change::Started);
    gate.setMode(TransmitGate::Mode::PushToTalk);
    REQUIRE(gate.step(true) == TransmitGate::Change::Stopped);
    REQUIRE_FALSE(gate.transmitting());
}
