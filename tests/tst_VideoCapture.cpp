// The sizing and occlusion cases are upstream's (tst_VideoEncode.cpp, ouwou/acheron 71b4907).
#include "Core/Video/CameraCapture.hpp"
#include "Core/Video/ScreenCapture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace Styx::Core::Video;

TEST_CASE("A source is fitted inside the send size", "[video][capture]")
{
    CHECK(encodeSizeFor({1920, 1080}, 720) == FrameSize{1280, 720});
    CHECK(encodeSizeFor({2560, 1440}, 1080) == FrameSize{1920, 1080});
    CHECK(encodeSizeFor({3840, 2160}, 0) == FrameSize{3840, 2160});
    // Never enlarged.
    CHECK(encodeSizeFor({1280, 720}, 1080) == FrameSize{1280, 720});
    CHECK(encodeSizeFor({1366, 768}, 1080) == FrameSize{1366, 768});
    // Wider than 16:9 is held by the width, taller by the height.
    CHECK(encodeSizeFor({3440, 1440}, 720) == FrameSize{1280, 534});
    CHECK(encodeSizeFor({1440, 2560}, 720) == FrameSize{404, 720});
    CHECK(encodeSizeFor({1920, 1200}, 720) == FrameSize{1152, 720});
    // Both sides even, also when nothing is scaled.
    CHECK(encodeSizeFor({1365, 767}, 0) == FrameSize{1364, 766});
    CHECK(encodeSizeFor({1920, 1080}, 480) == FrameSize{852, 480});
}

TEST_CASE("A still picture is sent again on its interval, or sooner for a key frame", "[video][capture]")
{
    CHECK_FALSE(stillPictureIsDue(std::chrono::milliseconds(32), true, 30));
    CHECK(stillPictureIsDue(std::chrono::milliseconds(34), true, 30));
    CHECK_FALSE(stillPictureIsDue(std::chrono::milliseconds(499), false, 30));
    CHECK(stillPictureIsDue(std::chrono::milliseconds(500), false, 30));
}

// The official client's rule for when a window cannot be cut out of the screen by itself.
TEST_CASE("A window above covers the one below", "[video][capture]")
{
    const auto at = [](int x, int y, int width, int height) {
        StackedWindow window;
        window.bounds = {x, y, width, height};
        return window;
    };
    const StackedWindow target = at(100, 100, 400, 300);

    CHECK_FALSE(windowIsCovered({target}, 0));
    CHECK(windowIsCovered({at(300, 200, 400, 300), target}, 1));
    // By a single pixel as well, and by a window that lies wholly inside it.
    CHECK(windowIsCovered({at(499, 399, 50, 50), target}, 1));
    CHECK(windowIsCovered({at(200, 200, 10, 10), target}, 1));
    // One that only touches it, or is somewhere else, does not.
    CHECK_FALSE(windowIsCovered({at(500, 100, 200, 300), target}, 1));
    CHECK_FALSE(windowIsCovered({at(100, 400, 400, 50), target}, 1));
    CHECK_FALSE(windowIsCovered({at(900, 900, 200, 200), at(0, 0, 100, 100), target}, 2));

    // Only what is above counts, however much of it what is below would hide.
    CHECK_FALSE(windowIsCovered({target, at(0, 0, 2000, 2000)}, 0));
    CHECK(windowIsCovered({at(0, 0, 2000, 2000), target, at(0, 0, 2000, 2000)}, 1));

    // What does not show on the screen covers nothing, nor does an overlay.
    for (int ignored = 0; ignored < 6; ignored++) {
        INFO(ignored);
        StackedWindow above = at(0, 0, 2000, 2000);
        above.visible = ignored != 0;
        above.minimised = ignored == 1;
        above.cloaked = ignored == 2;
        above.overlay = ignored == 3;
        above.tabProxy = ignored == 4;
        if (ignored == 5)
            above.bounds = {};
        CHECK_FALSE(windowIsCovered({above, target}, 1));
    }
    // A tool window and a window of the same program are windows like any other.
    CHECK(windowIsCovered({at(0, 0, 2000, 2000), at(150, 150, 20, 20), target}, 2));

    // The taskbar's full-size preview shows over everything wherever it says it is.
    StackedWindow preview = at(5000, 5000, 10, 10);
    preview.livePreview = true;
    CHECK(windowIsCovered({preview, target}, 1));
    preview.visible = false;
    CHECK_FALSE(windowIsCovered({preview, target}, 1));

    // A window that shows nothing of itself cannot be cut out of the screen either.
    StackedWindow gone = target;
    gone.minimised = true;
    CHECK(windowIsCovered({gone}, 0));
    gone = target;
    gone.cloaked = true;
    CHECK(windowIsCovered({gone}, 0));
    gone = target;
    gone.bounds = {100, 100, 0, 0};
    CHECK(windowIsCovered({gone}, 0));
    CHECK(windowIsCovered({target}, 1));
    CHECK(windowIsCovered({}, 0));
}

TEST_CASE("The capture clock keeps growing through a camera clock reset", "[video][capture]")
{
    CaptureClock clock;
    const auto now = std::chrono::steady_clock::now();
    const auto first = clock.at(std::chrono::microseconds(1000), now);
    const auto second = clock.at(std::chrono::microseconds(0), now + std::chrono::milliseconds(2));
    CHECK(second > first);
}
