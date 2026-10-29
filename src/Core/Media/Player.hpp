#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Core/ProxyConfig.hpp"
#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include "Platform/Trackable.hpp"

namespace Styx::Core::Media {

enum class MediaKind { Video, Audio };

bool isSupported();
bool canPlay(const std::string &contentType, const std::string &url);
bool canPlayAudio(const std::string &contentType, const std::string &url);
float storedVolume();
void setStoredVolume(float volume);
bool storedMuted();
void setStoredMuted(bool muted);

class Player : public Platform::Trackable {
public:
    enum class State { Idle, Opening, Playing, Paused, Ended, Error };

    Player();
    ~Player() override;
    void open(const std::string &url, const ProxyConfig &proxy);
    void close();
    void play();
    void pause();
    void togglePlayPause();
    void seek(int64_t positionMs);
    void setVolume(float volume);
    [[nodiscard]] float volume() const;
    void setMuted(bool muted);
    [[nodiscard]] bool isMuted() const;
    void setTargetSize(const Platform::Size &size);
    [[nodiscard]] Platform::ImageBuffer currentFrame() const;
    [[nodiscard]] int64_t position() const;
    [[nodiscard]] int64_t duration() const;
    [[nodiscard]] int64_t bufferedPosition() const;
    [[nodiscard]] Platform::Size nativeSize() const;
    [[nodiscard]] bool hasVideo() const;
    [[nodiscard]] State state() const;
    [[nodiscard]] bool isPlaying() const { return state() == State::Playing; }
    [[nodiscard]] bool isSeekable() const;

    Platform::Signal<> frameReady;
    Platform::Signal<int64_t> positionChanged;
    Platform::Signal<int64_t> durationChanged;
    Platform::Signal<State> stateChanged;
    Platform::Signal<std::string> errorOccurred;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Styx::Core::Media
