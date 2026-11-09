#pragma once

#include "Platform/Settings.hpp"

#include <string>
#include <string_view>

namespace Styx::Core::Settings {

namespace Keys {
// General / System
inline constexpr const char *InMemoryCache = "general/in_memory_cache";
inline constexpr const char *CloseToTray = "general/close_to_tray";

// Chat / Media
inline constexpr const char *AnimateEmoji = "chat/animate_emoji";
inline constexpr const char *AnimateStickers = "chat/animate_stickers";
inline constexpr const char *GifPlayback = "chat/gif_playback";
inline constexpr const char *GifMaxPlaying = "chat/gif_max_playing";
inline constexpr const char *GifMaxMb = "chat/gif_max_mb";
inline constexpr const char *AnimationCacheMb = "chat/animation_cache_mb";
inline constexpr const char *AnimationMaxMb = "chat/animation_max_mb";

// Notifications
inline constexpr const char *DesktopNotifications = "notifications/desktop";
inline constexpr const char *FlashTaskbar = "notifications/flash_taskbar";
inline constexpr const char *TaskbarBadge = "notifications/taskbar_badge";

// Theme / Appearance
inline constexpr const char *ThemeUseSystemColors = "appearance/use_system_colors";

// Audio & Voice
inline constexpr const char *AudioBackend = "audio/backend";
inline constexpr const char *VoiceInputDevice = "voice/input_device";
inline constexpr const char *VoiceOutputDevice = "voice/output_device";
inline constexpr const char *VoiceInputVolume = "voice/input_volume";
inline constexpr const char *VoiceOutputVolume = "voice/output_volume";
inline constexpr const char *VoiceInputMode = "voice/input_mode";
inline constexpr const char *VoicePushToTalkKey = "voice/ptt_key";
inline constexpr const char *VoicePushToTalkReleaseDelay = "voice/ptt_release_delay";

// Stream / Go Live
inline constexpr const char *StreamQuality = "stream/quality";
inline constexpr const char *StreamAudio = "stream/audio";
} // namespace Keys

namespace Defaults {
// General
inline constexpr bool InMemoryCache = false;
inline constexpr bool CloseToTray = false;

// Chat
inline constexpr bool AnimateEmoji = true;
inline constexpr bool AnimateStickers = true;
inline constexpr const char *GifPlayback = "focused";
inline constexpr int GifMaxPlaying = 4;
inline constexpr int GifMaxMb = 32;
inline constexpr int AnimationCacheMb = 128;
inline constexpr int AnimationMaxMb = 16;

// Notifications
inline constexpr bool DesktopNotifications = true;
inline constexpr bool FlashTaskbar = true;
inline constexpr bool TaskbarBadge = true;

// Theme
inline constexpr bool ThemeUseSystemColors = false;

// Audio & Voice
inline constexpr const char *AudioBackend = "";
inline constexpr const char *VoiceInputDevice = "";
inline constexpr const char *VoiceOutputDevice = "";
inline constexpr int VoiceInputVolume = 100;
inline constexpr int VoiceOutputVolume = 100;
// "voice_activity" or "push_to_talk".
inline constexpr const char *VoiceInputMode = "voice_activity";
// Milliseconds, as in the official client's slider (0 to 2000, default 20).
inline constexpr int VoicePushToTalkReleaseDelay = 20;

// Stream
inline constexpr const char *StreamQuality = "720p30";
inline constexpr bool StreamAudio = true;
} // namespace Defaults

inline nlohmann::json get(std::string_view key, const nlohmann::json &defaultValue = {})
{
    return Platform::Settings::instance().getJson(key, defaultValue);
}

inline bool getBool(std::string_view key, bool defaultValue)
{
    return Platform::Settings::instance().getBool(key, defaultValue);
}

inline int getInt(std::string_view key, int defaultValue)
{
    return Platform::Settings::instance().getInt(key, defaultValue);
}

inline std::string getString(std::string_view key, std::string_view defaultValue = {})
{
    return Platform::Settings::instance().getString(key, defaultValue);
}

inline void set(std::string_view key, const nlohmann::json &value)
{
    Platform::Settings::instance().setJson(key, value);
}

inline void setBool(std::string_view key, bool value)
{
    Platform::Settings::instance().setBool(key, value);
}

inline void setInt(std::string_view key, int value)
{
    Platform::Settings::instance().setInt(key, value);
}

inline void setString(std::string_view key, std::string_view value)
{
    Platform::Settings::instance().setString(key, value);
}

} // namespace Styx::Core::Settings
