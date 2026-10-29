#include "UI/Chat/GifPlayback.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

namespace Styx::UI {
namespace {
std::atomic<uint64_t> nextPlayback{0};
constexpr int64_t BytesPerMiB = 1024 * 1024;
}

GifPlayMode gifPlayModeFromSetting(const std::string &value)
{
    if (value == "hover") return GifPlayMode::OnHover;
    if (value == "never") return GifPlayMode::Never;
    return GifPlayMode::WhenFocused;
}

GifPlayback::GifPlayback(Core::ImageManager *manager, AccountSource accountSource,
                         SetFrame frameSink, ClearFrame frameClear)
    : images(manager), account(std::move(accountSource)), setFrame(std::move(frameSink)),
      clearFrame(std::move(frameClear)), animations(manager), prefix("gif-" + std::to_string(++nextPlayback) + "-")
{
    player.setMemoryLimitForNewClips(memoryMiB * BytesPerMiB);
    track(player.frameChanged.connect([this](auto id) {
        auto it = clipKeys.find(id);
        if (it != clipKeys.end()) {
            if (const auto *frame = player.frame(id); frame && setFrame)
                setFrame(frameKey(it->second), *frame);
        }
    }));
    track(player.clipFailed.connect([this](auto id) {
        auto it = clipKeys.find(id);
        if (it == clipKeys.end()) return;
        const auto key = it->second;
        auto surface = surfaces.find(key);
        if (surface != surfaces.end()) {
            failed.insert(surface->second.url);
            close(surface->second, key);
        }
        schedule();
    }));
    track(animations.framesReady.connect([this](const std::string &url, const Platform::Size &size, const Core::AnimatedFramesPtr &frames) {
        for (auto &[key, surface] : surfaces)
            if (surface.decodedAnimation && surface.url == url && surface.size == size) surface.frames = frames;
        reconcile();
    }));
    if (manager) {
        track(manager->rawDownloadReady.connect([this](const std::string &, const Platform::Size &) { schedule(); }));
        track(manager->imageUnavailable.connect([this](const std::string &url, const Platform::Size &size) {
            if (!size.empty()) return;
            failed.insert(url);
            retryTimer.start(std::chrono::milliseconds(Core::ImageManager::FailedFetchRetryMs + 2000), false, [this] {
                failed.clear();
                reconcile();
            });
        }));
    }
}

GifPlayback::~GifPlayback() { reset(); }

std::string GifPlayback::frameKey(const std::string &key) const { return prefix + key; }

void GifPlayback::setPlayMode(GifPlayMode value)
{
    if (mode == value) return;
    mode = value;
    reconcile();
}
void GifPlayback::setMaxPlayingAtOnce(int count)
{
    maxPlaying = std::clamp(count, FewestPlayingAtOnce, MostPlayingAtOnce);
    reconcile();
}
void GifPlayback::setClipMemoryLimitMiB(int mib)
{
    memoryMiB = std::clamp(mib, ClipMemoryLimit.min, ClipMemoryLimit.max);
    player.setMemoryLimitForNewClips(memoryMiB * BytesPerMiB);
    failed.clear();
    reconcile();
}
void GifPlayback::setFocused(bool value) { focused = value; reconcile(); }
void GifPlayback::setViewVisible(bool value) { viewVisible = value; reconcile(); }
std::string GifPlayback::show(const std::string &key, const std::string &url, Platform::Size size,
                              bool visible, bool hovered, ClipFit fit, bool decodedAnimation)
{
    seen.insert(key);
    auto &surface = surfaces[key];
    if (surface.url != url || surface.size != size || surface.fit != fit || surface.decodedAnimation != decodedAnimation) {
        close(surface, key);
        surface.decodedAnimation = decodedAnimation;
        surface.frames.reset();
        surface.url = url;
        surface.size = size;
        surface.fit = fit;
    }
    surface.visible = visible;
    surface.hovered = hovered;
    return "frame:" + frameKey(key);
}

void GifPlayback::endUpdate()
{
    for (auto it = surfaces.begin(); it != surfaces.end();) {
        if (!seen.contains(it->first)) {
            close(it->second, it->first);
            it = surfaces.erase(it);
        } else ++it;
    }
    schedule();
}

void GifPlayback::close(Surface &surface, const std::string &key)
{
    if (surface.clip) {
        clipKeys.erase(surface.clip);
        player.close(surface.clip);
        surface.clip = 0;
    }
    if (clearFrame) clearFrame(frameKey(key));
}

void GifPlayback::schedule()
{
    settleTimer.start(std::chrono::milliseconds(120), false, [this] { reconcile(); });
}

void GifPlayback::reconcile()
{
    int count = 0;
    bool paused = false;
    for (auto &[key, surface] : surfaces) {
        const bool wanted = viewVisible && focused && surface.visible && mode != GifPlayMode::Never &&
                            (mode != GifPlayMode::OnHover || surface.hovered) && count < maxPlaying;
        if (!wanted) {
            if (surface.clip) { player.setPlaying(surface.clip, false); paused = true; }
            continue;
        }
        if (surface.decodedAnimation) {
            ++count;
            if (!surface.frames && images && !surface.url.empty() && !surface.size.empty())
                surface.frames = animations.get(surface.url, surface.size, account ? account() : Core::Snowflake::Invalid);
            continue;
        }
        ++count;
        if (surface.clip) { player.setPlaying(surface.clip, true); continue; }
        if (!images || surface.url.empty() || surface.size.empty() || failed.contains(surface.url)) continue;
        const Platform::Size downloaded{};
        const auto path = images->rawDownloadPath(surface.url, downloaded);
        if (path.empty()) {
            images->downloadToCache(surface.url, downloaded, account ? account() : Core::Snowflake::Invalid);
            continue;
        }
        auto fit = surface.fit == ClipFit::Crop ? Core::Media::ClipPlayer::Fit::Cover : Core::Media::ClipPlayer::Fit::Contain;
        surface.clip = player.open(path, surface.size, fit);
        if (surface.clip) {
            clipKeys.emplace(surface.clip, key);
            player.setPlaying(surface.clip, true);
        } else {
            failed.insert(surface.url);
        }
    }
    if (paused && !pausedTimer.active()) {
        pausedTimer.start(std::chrono::seconds(30), false, [this] {
            for (auto &[key, surface] : surfaces) {
                const bool wanted = viewVisible && focused && surface.visible && mode != GifPlayMode::Never &&
                                    (mode != GifPlayMode::OnHover || surface.hovered);
                if (!wanted) close(surface, key);
            }
        });
    } else if (!paused) {
        pausedTimer.stop();
    }

    presentAnimations();
    if (viewVisible && focused && mode != GifPlayMode::Never) {
        if (!animationTimer.active())
            animationTimer.start(std::chrono::milliseconds(33), true, [this] { presentAnimations(); });
    } else {
        animationTimer.stop();
    }
}

void GifPlayback::presentAnimations()
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - animationStart).count();
    int count = 0;
    for (auto &[key, surface] : surfaces) {
        const bool wanted = viewVisible && focused && surface.visible && mode != GifPlayMode::Never &&
                            (mode != GifPlayMode::OnHover || surface.hovered) && count < maxPlaying;
        if (!wanted) continue;
        ++count;
        if (!surface.decodedAnimation || !surface.frames || surface.frames->frames.empty() || !setFrame) continue;
        const auto loop = surface.frames->loopMs();
        const auto index = surface.frames->frameAt(loop > 0 ? elapsed % loop : 0);
        if (index >= 0 && static_cast<size_t>(index) < surface.frames->frames.size())
            setFrame(frameKey(key), surface.frames->frames[index]);
    }
}

void GifPlayback::reset()
{
    settleTimer.stop(); retryTimer.stop(); pausedTimer.stop(); animationTimer.stop();
    for (auto &[key, surface] : surfaces) close(surface, key);
    surfaces.clear(); clipKeys.clear(); seen.clear(); failed.clear();
}

} // namespace Styx::UI
