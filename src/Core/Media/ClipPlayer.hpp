#pragma once

#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Styx::Core::Media {

class ClipPlayer : public Platform::Trackable {
public:
    using ClipId = uint64_t;
    enum class Fit { Contain, Cover, Stretch };

    ClipPlayer();
    ~ClipPlayer() override;
    [[nodiscard]] static int64_t memoryToPlay(Platform::Size nativeSize, Platform::Size frameSize);
    void setMemoryLimitForNewClips(int64_t bytes);
    ClipId open(const std::string &path, Platform::Size boundPixels, Fit fit);
    void close(ClipId id);
    void closeAll();
    void setPlaying(ClipId id, bool playing);
    [[nodiscard]] const Platform::ImageBuffer *frame(ClipId id) const;
    Platform::Signal<ClipId> frameChanged;
    Platform::Signal<ClipId> clipFailed;

private:
    struct Clip;
    using ClipPtr = std::shared_ptr<Clip>;
    void decodeLoop();
    [[nodiscard]] ClipPtr nextClipAwaitingFrame();
    [[nodiscard]] static std::optional<int64_t> decodeNextFrame(Clip &clip, Platform::ImageBuffer &buffer);
    void requestPresent();
    void presentDueFrames();
    [[nodiscard]] int64_t elapsedMs() const;

    std::unordered_map<ClipId, ClipPtr> clips;
    ClipId lastId = 0;
    int64_t newClipMemoryLimitBytes = std::numeric_limits<int64_t>::max();
    Platform::Timer presentTimer;
    std::chrono::steady_clock::time_point clock = std::chrono::steady_clock::now();
    std::thread decodeThread;
    std::mutex mutex;
    std::condition_variable frameWanted;
    std::vector<ClipPtr> decodeOrder;
    size_t nextToServe = 0;
    bool stopping = false;
    std::atomic<bool> presentRequested{false};
    std::shared_ptr<void> callbackLifetime = std::make_shared<int>(0);
};

} // namespace Styx::Core::Media
