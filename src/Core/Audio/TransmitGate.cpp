#include "Core/Audio/TransmitGate.hpp"

#include <algorithm>

namespace Styx::Core::Audio {

void TransmitGate::setMode(Mode mode)
{
    if (currentMode != mode) {
        currentMode = mode;
        keyHeld = false;
        holdRemaining = 0;
    }
}

void TransmitGate::setReleaseDelayMs(int ms)
{
    const int clamped = std::clamp(ms, 0, MaxReleaseDelayMs);
    releaseFrames = (clamped + FrameMs - 1) / FrameMs;
}

void TransmitGate::setKeyHeld(bool held)
{
    keyHeld = held;
}

TransmitGate::Change TransmitGate::step(bool voiceDetected)
{
    const bool active = (currentMode == Mode::PushToTalk) ? keyHeld : voiceDetected;
    const int hold = (currentMode == Mode::PushToTalk) ? releaseFrames : VoiceActivityHoldFrames;

    if (active) {
        holdRemaining = hold;
        if (!open) {
            open = true;
            return Change::Started;
        }
    } else if (holdRemaining > 0) {
        holdRemaining--;
    } else if (open) {
        open = false;
        return Change::Stopped;
    }

    return Change::None;
}

bool TransmitGate::close()
{
    const bool was = open;
    open = false;
    holdRemaining = 0;
    return was;
}

} // namespace Styx::Core::Audio
