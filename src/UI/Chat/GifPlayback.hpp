#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Core/ImageManager.hpp"
#include "Core/AnimatedImageCache.hpp"
#include "Core/Media/ClipPlayer.hpp"
#include "Core/MiBRange.hpp"
#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"

namespace Styx::UI {

enum class ClipFit { Fit, Crop };
enum class GifPlayMode { WhenFocused, OnHover, Never };
[[nodiscard]] GifPlayMode gifPlayModeFromSetting(const std::string &value);

// The caller records the media currently in the document between beginUpdate and
// endUpdate. Keys must identify a particular element and remain stable across updates.
class GifPlayback : public Platform::Trackable {
public:
    static constexpr int FewestPlayingAtOnce = 1;
    static constexpr int DefaultPlayingAtOnce = 8;
    static constexpr int MostPlayingAtOnce = 32;
    static constexpr Core::MiBRange ClipMemoryLimit{8, 48, 256};
    using AccountSource = std::function<Core::Snowflake()>;
    using SetFrame = std::function<void(const std::string &, const Platform::ImageBuffer &)>;
    using ClearFrame = std::function<void(const std::string &)>;

    GifPlayback(Core::ImageManager *images, AccountSource account, SetFrame setFrame, ClearFrame clearFrame);
    ~GifPlayback() override;
    void setPlayMode(GifPlayMode mode);
    void setMaxPlayingAtOnce(int count);
    void setClipMemoryLimitMiB(int mib);
    void setFocused(bool focused);
    void setViewVisible(bool visible);
    void beginUpdate();
    std::string show(const std::string &key, const std::string &url, Platform::Size size,
                     bool visible, bool hovered, ClipFit fit = ClipFit::Fit, bool decodedAnimation = false);
    void endUpdate();
    void reset();

private:
    struct Surface {
        std::string url;
        Platform::Size size;
        ClipFit fit = ClipFit::Fit;
        bool visible = false;
        bool hovered = false;
        bool decodedAnimation = false;
        Core::AnimatedFramesPtr frames;
        Core::Media::ClipPlayer::ClipId clip = 0;
    };
    void schedule();
    void reconcile();
    void close(Surface &surface, const std::string &key);
    std::string frameKey(const std::string &key) const;

    void presentAnimations();
    Platform::Guard<Core::ImageManager> images;
    AccountSource account;
    SetFrame setFrame;
    ClearFrame clearFrame;
    Core::Media::ClipPlayer player;
    std::unordered_map<std::string, Surface> surfaces;
    std::unordered_map<Core::Media::ClipPlayer::ClipId, std::string> clipKeys;
    std::unordered_set<std::string> seen;
    Core::AnimatedImageCache animations;
    std::unordered_set<std::string> failed;
    std::string prefix;
    GifPlayMode mode = GifPlayMode::WhenFocused;
    int maxPlaying = DefaultPlayingAtOnce;
    Platform::Timer settleTimer, retryTimer, pausedTimer, animationTimer;
    std::chrono::steady_clock::time_point animationStart = std::chrono::steady_clock::now();
    int memoryMiB = ClipMemoryLimit.fallback;
    bool focused = true;
    bool viewVisible = false;
};

} // namespace Styx::UI
