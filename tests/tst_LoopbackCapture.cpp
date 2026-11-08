#ifndef NOMINMAX
#define NOMINMAX 1
#endif
// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
// The capture against this machine's own sound: the test plays a short, quiet tone and looks
// for it in what is delivered. Then the rules that cut captured sound into runs, which need
// nothing of the machine.

#include "Core/Audio/LoopbackCapture.hpp"
#include "Core/Streams/StreamAudio.hpp"
#include "HardwareTests.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) || defined(__linux__)
#define MINIAUDIO_IMPLEMENTATION
#include "Core/Audio/Miniaudio.hpp"
#undef min
#undef max
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#elif defined(__linux__)
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace Styx;
using namespace Styx::Core;
using Audio::LoopbackCapture;

namespace {

constexpr double TONE_HZ = 1375;
constexpr float TONE_AMPLITUDE = 0.02f;
constexpr int TONE_MS = 400;
constexpr int TONE_FADE_MS = 15;
// For Windows to have made the capture before there is anything to hear.
constexpr int CAPTURE_STARTS_MS = 250;
constexpr int SOUND_ARRIVES_MS = 200;

const char CANNOT_CAPTURE[] = "This machine cannot capture one program's sound.";
const char CANNOT_PLAY[] = "This machine has no sound output to play a tone on.";

#if defined(_WIN32) || defined(__linux__)

// Plays the tone once on the default output, from this process, then silence.
class Tone {
public:
    Tone()
    {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = Audio::LOOPBACK_CHANNELS;
        config.sampleRate = Audio::LOOPBACK_SAMPLE_RATE;
        config.dataCallback = &Tone::fill;
        config.pUserData = this;
#ifdef _WIN32
        // Only through WASAPI, which is where the capture listens. Left to choose, miniaudio
        // ends at a backend that plays to nowhere on a machine with no sound card.
        const ma_backend backends[] = { ma_backend_wasapi };
        hasContext = ma_context_init(backends, 1, nullptr, &context) == MA_SUCCESS;
#else
        hasContext = ma_context_init(nullptr, 0, nullptr, &context) == MA_SUCCESS;
#endif
        opened = hasContext && ma_device_init(&context, &config, &device) == MA_SUCCESS;
        playing = opened && ma_device_start(&device) == MA_SUCCESS;
    }

    ~Tone()
    {
        if (opened)
            ma_device_uninit(&device);
        if (hasContext)
            ma_context_uninit(&context);
    }

    [[nodiscard]] bool isPlaying() const { return playing; }

private:
    static void fill(ma_device *device, void *output, const void *, ma_uint32 sampleFrames)
    {
        constexpr int LENGTH = TONE_MS * Audio::LOOPBACK_SAMPLE_RATE / 1000;
        constexpr float FADE = TONE_FADE_MS * Audio::LOOPBACK_SAMPLE_RATE / 1000;

        auto *tone = static_cast<Tone *>(device->pUserData);
        auto *samples = static_cast<float *>(output);
        for (ma_uint32 i = 0; i < sampleFrames; i++) {
            float sample = 0;
            if (tone->position < LENGTH) {
                // Faded in and out, to spare whoever is in the room the clicks.
                const float envelope = std::min(1.0f, std::min<float>(static_cast<float>(tone->position), static_cast<float>(LENGTH - tone->position)) / FADE);
                sample = TONE_AMPLITUDE * envelope * static_cast<float>(std::sin(2 * std::numbers::pi * TONE_HZ * tone->position / Audio::LOOPBACK_SAMPLE_RATE));
                tone->position++;
            }
            samples[2 * i] = sample;
            samples[2 * i + 1] = sample;
        }
    }

    ma_context context{};
    bool hasContext = false;
    ma_device device{};
    bool opened = false;
    bool playing = false;
    int position = 0;
};

#else

class Tone {
public:
    [[nodiscard]] bool isPlaying() const { return false; }
};

#endif

#ifdef _WIN32

int threadsOfThisProcess()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == GetCurrentProcessId())
            count++;
    }
    CloseHandle(snapshot);
    return count;
}

bool processExists(std::uint32_t processId)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process)
        CloseHandle(process);
    return process != nullptr;
}

#elif defined(__linux__)

int threadsOfThisProcess()
{
    int count = 0;
    if (DIR *d = opendir("/proc/self/task")) {
        while (dirent *de = readdir(d)) {
            if (de->d_name[0] != '.')
                count++;
        }
        closedir(d);
    }
    return count;
}

bool processExists(std::uint32_t processId)
{
    struct stat s{};
    return stat(("/proc/" + std::to_string(processId)).c_str(), &s) == 0;
}

#else

int threadsOfThisProcess() { return 0; }
bool processExists(std::uint32_t) { return false; }

#endif

// What one capture delivers, gathered on the test's thread.
struct Recording {
    Recording()
    {
        frameConn = capture.frameCaptured.connect([this](std::vector<std::uint8_t> pcm) {
            std::lock_guard lock(mutex);
            frames.push_back(std::move(pcm));
        });
        failConn = capture.failed.connect([this](std::string reason) {
            std::lock_guard lock(mutex);
            failures.push_back(std::move(reason));
        });
    }

    [[nodiscard]] bool everyFrameIsWhole() const
    {
        std::lock_guard lock(mutex);
        return std::all_of(frames.cbegin(), frames.cend(), [](const std::vector<std::uint8_t> &pcm) {
            return pcm.size() == Audio::LOOPBACK_FRAME_SIZE;
        });
    }

    // The frames with something in them, end to end.
    [[nodiscard]] std::vector<std::uint8_t> sound() const
    {
        std::lock_guard lock(mutex);
        std::vector<std::uint8_t> joined;
        for (const auto &pcm : frames) {
            if (!isSilence(pcm))
                joined.insert(joined.end(), pcm.begin(), pcm.end());
        }
        return joined;
    }

    [[nodiscard]] std::string failureText() const
    {
        std::lock_guard lock(mutex);
        std::string res;
        for (const auto &f : failures) {
            if (!res.empty())
                res += " ";
            res += f;
        }
        return res;
    }

    LoopbackCapture capture;
    Platform::Connection frameConn;
    Platform::Connection failConn;
    mutable std::mutex mutex;
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::string> failures;
};

// How much of one frequency there is in 16-bit stereo sound, as the amplitude of a steady tone.
double level(const std::vector<std::uint8_t> &pcm, double hz)
{
    const auto *samples = reinterpret_cast<const std::int16_t *>(pcm.data());
    const int sampleFrames = static_cast<int>(pcm.size()) / (Audio::LOOPBACK_CHANNELS * Audio::LOOPBACK_SAMPLE_BYTES);
    if (sampleFrames == 0)
        return 0;

    double real = 0;
    double imaginary = 0;
    for (int i = 0; i < sampleFrames; i++) {
        const double mono = (samples[2 * i] + samples[2 * i + 1]) / 2.0;
        const double angle = 2 * std::numbers::pi * hz * i / Audio::LOOPBACK_SAMPLE_RATE;
        real += mono * std::cos(angle);
        imaginary += mono * std::sin(angle);
    }
    return 2 * std::hypot(real, imaginary) / sampleFrames;
}

// Other programs may be playing anything, so the tone is told by standing out of what is
// beside it as well as by its size.
bool hasTone(const std::vector<std::uint8_t> &pcm)
{
    constexpr double BESIDE_HZ = 97;
    const double tone = level(pcm, TONE_HZ);
    const double beside = std::max(level(pcm, TONE_HZ - BESIDE_HZ), level(pcm, TONE_HZ + BESIDE_HZ));
    return tone > 0.3 * TONE_AMPLITUDE * 32767 && tone > 4 * beside;
}

std::vector<std::uint8_t> frameOf(std::int16_t sample)
{
    std::vector<std::uint8_t> pcm(Audio::LOOPBACK_FRAME_SIZE);
    std::fill_n(reinterpret_cast<std::int16_t *>(pcm.data()), pcm.size() / sizeof(std::int16_t), sample);
    return pcm;
}

} // namespace

TEST_CASE("Whether loopback capture is available is found out once", "[audio][loopback]")
{
    const bool available = Audio::processLoopbackAvailable();
#if !defined(_WIN32) && !defined(STYX_HAVE_PIPEWIRE)
    CHECK_FALSE(available);
#endif

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; i++)
        CHECK(Audio::processLoopbackAvailable() == available);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed < 100);
}

TEST_CASE("What this program plays is captured", "[audio][loopback]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    if (!Audio::processLoopbackAvailable())
        SKIP(CANNOT_CAPTURE);
#ifdef _WIN32
    const auto self = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    const auto self = static_cast<std::uint32_t>(getpid());
#endif

    // Stopped and started again before there is anything to hear, so what is heard is the second start's.
    Recording own;
    own.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    std::this_thread::sleep_for(std::chrono::milliseconds(CAPTURE_STARTS_MS));
    own.capture.stop();
    own.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    std::this_thread::sleep_for(std::chrono::milliseconds(CAPTURE_STARTS_MS));

    Tone tone;
    if (!tone.isPlaying())
        SKIP(CANNOT_PLAY);
    std::this_thread::sleep_for(std::chrono::milliseconds(TONE_MS + SOUND_ARRIVES_MS));
    own.capture.stop();

    CHECK(own.failureText().empty());
    CHECK(own.everyFrameIsWhole());
    const int framesOfTone = TONE_MS / Audio::LOOPBACK_FRAME_DURATION_MS;
    const int framesOfSound = static_cast<int>(own.sound().size()) / Audio::LOOPBACK_FRAME_SIZE;
    CHECK(framesOfSound >= framesOfTone - 2);
    CHECK(framesOfSound <= framesOfTone + 2);
    CHECK(hasTone(own.sound()));
}

TEST_CASE("Everything but this program leaves its tone out", "[audio][loopback]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    if (!Audio::processLoopbackAvailable())
        SKIP(CANNOT_CAPTURE);
#ifdef _WIN32
    const auto self = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    const auto self = static_cast<std::uint32_t>(getpid());
#endif

    // Both at once, so that the tone the one must not have is known to have been there.
    Recording own;
    Recording others;
    own.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    others.capture.start(self, LoopbackCapture::Scope::AllButProcessTree);
    std::this_thread::sleep_for(std::chrono::milliseconds(CAPTURE_STARTS_MS));

    Tone tone;
    if (!tone.isPlaying())
        SKIP(CANNOT_PLAY);
    std::this_thread::sleep_for(std::chrono::milliseconds(TONE_MS + SOUND_ARRIVES_MS));
    own.capture.stop();
    others.capture.stop();

    CHECK(own.failureText().empty());
    CHECK(others.failureText().empty());
    CHECK(hasTone(own.sound()));
    CHECK(others.everyFrameIsWhole());
    CHECK_FALSE(hasTone(others.sound()));
}

TEST_CASE("A process that does not exist has no sound", "[audio][loopback]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    if (!Audio::processLoopbackAvailable())
        SKIP(CANNOT_CAPTURE);
    constexpr std::uint32_t NOBODY = 0x7FFFFFF0;
    REQUIRE_FALSE(processExists(NOBODY));

    Recording nobody;
    nobody.capture.start(NOBODY, LoopbackCapture::Scope::ProcessTree);
    std::this_thread::sleep_for(std::chrono::milliseconds(CAPTURE_STARTS_MS));

    Tone tone;
    if (!tone.isPlaying())
        SKIP(CANNOT_PLAY);
    std::this_thread::sleep_for(std::chrono::milliseconds(TONE_MS + SOUND_ARRIVES_MS));
    nobody.capture.stop();

    CHECK(nobody.failureText().empty());
    CHECK(nobody.everyFrameIsWhole());
    CHECK(nobody.sound().empty());
#ifndef _WIN32
    CHECK(nobody.frames.size() >= static_cast<size_t>(TONE_MS / Audio::LOOPBACK_FRAME_DURATION_MS - 2));
#endif
}

TEST_CASE("Starting and stopping leaves no thread behind", "[audio][loopback]")
{
    if (!Audio::processLoopbackAvailable())
        SKIP(CANNOT_CAPTURE);
#ifdef _WIN32
    const auto self = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    const auto self = static_cast<std::uint32_t>(getpid());
#endif

    const auto startAndStop = [self] {
        LoopbackCapture capture;
        for (int wait = 0; wait <= 30; wait += 10) {
            capture.start(self, LoopbackCapture::Scope::AllButProcessTree);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait));
            capture.stop();
        }
        capture.start(self, LoopbackCapture::Scope::ProcessTree);
    };

    // Windows keeps a few threads of its own once sound has been touched.
    startAndStop();
    const int before = threadsOfThisProcess();

    constexpr int ROUNDS = 20;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < ROUNDS; i++)
        startAndStop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed < 10000);
    CHECK(threadsOfThisProcess() - before < ROUNDS / 2);
}

TEST_CASE("This program is not among the playing programs", "[audio][loopback]")
{
    if (!Audio::processLoopbackAvailable())
        SKIP(CANNOT_CAPTURE);
#ifdef _WIN32
    const auto self = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    const auto self = static_cast<std::uint32_t>(getpid());
#endif

    Tone tone;
    if (!tone.isPlaying())
        SKIP(CANNOT_PLAY);
    std::this_thread::sleep_for(std::chrono::milliseconds(SOUND_ARRIVES_MS));

    const auto start = std::chrono::steady_clock::now();
    const std::vector<Audio::PlayingProgram> programs = Audio::playingPrograms();
    const std::vector<Audio::PlayingProgram> again = Audio::playingPrograms();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed < 1000);

    REQUIRE(again.size() == programs.size());
    for (size_t i = 0; i < programs.size(); i++) {
        CHECK(programs[i].processId != self);
        CHECK(again[i].processId == programs[i].processId);
        CHECK(again[i].name == programs[i].name);
        if (i > 0) {
            CHECK(programs[i - 1].name <= programs[i].name);
        }
        CHECK(std::none_of(programs.begin(), programs.begin() + i, [&](const Audio::PlayingProgram &other) {
            return other.processId == programs[i].processId;
        }));
    }
}

TEST_CASE("Silence is what rounding leaves", "[audio][loopback]")
{
    CHECK(isSilence(std::vector<std::uint8_t>{}));
    CHECK(isSilence(frameOf(0)));
    CHECK(isSilence(frameOf(2)));
    CHECK(isSilence(frameOf(-6)));
    CHECK_FALSE(isSilence(frameOf(7)));
    CHECK_FALSE(isSilence(frameOf(-7)));

    // One click in a frame of nothing is something to hear, and so is the full scale.
    std::vector<std::uint8_t> click = frameOf(0);
    reinterpret_cast<std::int16_t *>(click.data())[Audio::LOOPBACK_FRAME_SIZE / sizeof(std::int16_t) / 2 - 1] = 1000;
    CHECK_FALSE(isSilence(click));
    CHECK_FALSE(isSilence(frameOf(-32768)));
}

TEST_CASE("A run starts with sound and outlasts a short silence", "[audio][loopback]")
{
    SoundRuns runs;
    CHECK(runs.offer(true) == SoundRuns::Step::Skip);
    CHECK(runs.offer(true) == SoundRuns::Step::Skip);

    CHECK(runs.offer(false) == SoundRuns::Step::Begin);
    CHECK(runs.offer(false) == SoundRuns::Step::Continue);
    for (int i = 0; i < SoundRuns::HELD_SILENT_FRAMES; i++)
        CHECK(runs.offer(true) == SoundRuns::Step::Continue);
    CHECK(runs.offer(true) == SoundRuns::Step::End);
    CHECK(runs.offer(true) == SoundRuns::Step::Skip);

    CHECK(runs.offer(false) == SoundRuns::Step::Begin);
}

TEST_CASE("Sound during the silence keeps the run open", "[audio][loopback]")
{
    SoundRuns runs;
    CHECK(runs.offer(false) == SoundRuns::Step::Begin);
    for (int i = 0; i < SoundRuns::HELD_SILENT_FRAMES; i++)
        CHECK(runs.offer(true) == SoundRuns::Step::Continue);
    CHECK(runs.offer(false) == SoundRuns::Step::Continue);

    // The silence is counted from the last of the sound.
    for (int i = 0; i < SoundRuns::HELD_SILENT_FRAMES; i++)
        CHECK(runs.offer(true) == SoundRuns::Step::Continue);
    CHECK(runs.offer(true) == SoundRuns::Step::End);
}

TEST_CASE("A run ends when frames stop coming", "[audio][loopback]")
{
    SoundRuns runs;
    CHECK_FALSE(runs.stop());

    CHECK(runs.offer(false) == SoundRuns::Step::Begin);
    CHECK(runs.stop());
    CHECK_FALSE(runs.stop());
    CHECK(runs.offer(true) == SoundRuns::Step::Skip);
    CHECK(runs.offer(false) == SoundRuns::Step::Begin);
}
