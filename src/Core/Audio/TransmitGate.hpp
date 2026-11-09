#pragma once

namespace Styx::Core::Audio {

// Decides, one 20 ms capture frame at a time, whether the microphone is transmitting. In voice
// activity mode it opens on detected voice and stays open for VoiceActivityHoldFrames after
// the voice stops; in push-to-talk mode it opens while the key is held and stays open for the
// release delay after it's let go.
class TransmitGate
{
public:
    enum class Mode {
        VoiceActivity,
        PushToTalk
    };
    enum class Change {
        None,
        Started,
        Stopped
    };

    static constexpr int FrameMs = 20;
    static constexpr int VoiceActivityHoldFrames = 25;
    static constexpr int MaxReleaseDelayMs = 2000;

    void setMode(Mode mode);
    // Clamped to 0..MaxReleaseDelayMs and rounded up to whole frames.
    void setReleaseDelayMs(int ms);
    void setKeyHeld(bool held);

    // Feeds one frame. voiceDetected is ignored in push-to-talk mode.
    Change step(bool voiceDetected);
    [[nodiscard]] bool transmitting() const { return open; }
    // Closes the gate at once, as when capture stops; true if it was open.
    bool close();

    [[nodiscard]] Mode mode() const { return currentMode; }
    [[nodiscard]] int releaseDelayFrames() const { return releaseFrames; }

private:
    Mode currentMode = Mode::VoiceActivity;
    int releaseFrames = 1;
    bool keyHeld = false;
    bool open = false;
    int holdRemaining = 0;
};

} // namespace Styx::Core::Audio
