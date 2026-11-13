#pragma once

#include "Platform/Settings.hpp"

#include <string>
#include <string_view>

namespace Styx::Core::Settings {

namespace Keys {
// General / System
inline constexpr const char *InMemoryCache = "general/in_memory_cache";
inline constexpr const char *CloseToTray = "general/close_to_tray";
inline constexpr const char *StartMinimized = "general/start_minimized";

// Chat / Media
inline constexpr const char *CompactMessages = "chat/compact_messages";
inline constexpr const char *ShowAvatars = "chat/show_avatars";
inline constexpr const char *ShowEmbeds = "chat/show_embeds";
inline constexpr const char *ShowReactions = "chat/show_reactions";
inline constexpr const char *UnderlineLinks = "chat/underline_links";
inline constexpr const char *ReducedMotion = "accessibility/reduced_motion";
inline constexpr const char *AnimationCacheMb = "chat/animation_cache_mb";
inline constexpr const char *AnimationMaxMb = "chat/animation_max_mb";
// Notifications
inline constexpr const char *DesktopNotifications = "notifications/desktop";
inline constexpr const char *FlashTaskbar = "notifications/flash_taskbar";
inline constexpr const char *TaskbarBadge = "notifications/taskbar_badge";
inline constexpr const char *SoundsEnabled = "sounds/enabled";

// Theme / Appearance
inline constexpr const char *ThemeUseSystemColors = "appearance/use_system_colors";
inline constexpr const char *ThemeSeedColor = "appearance/seed_color";
inline constexpr const char *ThemeScheme = "appearance/scheme";
inline constexpr const char *ThemeDarkMode = "appearance/dark_mode";
inline constexpr const char *ThemeChoice = "appearance/theme";
inline constexpr const char *ThemeFollowDevice = "appearance/follow_device";
inline constexpr const char *ThemeSync = "appearance/sync_theme";
inline constexpr const char *InterfaceZoom = "accessibility/zoom";

// Audio & Voice
inline constexpr const char *AudioBackend = "audio/backend";
inline constexpr const char *VoiceInputDevice = "voice/input_device";
inline constexpr const char *VoiceOutputDevice = "voice/output_device";
inline constexpr const char *VoiceInputVolume = "voice/input_volume";
inline constexpr const char *VoiceOutputVolume = "voice/output_volume";
inline constexpr const char *VoiceInputMode = "voice/input_mode";
inline constexpr const char *VoicePushToTalkKey = "voice/ptt_key";
inline constexpr const char *VoiceToggleMuteKey = "voice/toggle_mute_key";
inline constexpr const char *VoiceToggleDeafenKey = "voice/toggle_deafen_key";
inline constexpr const char *VoicePushToTalkReleaseDelay = "voice/ptt_release_delay";

// Stream / Go Live
inline constexpr const char *StreamQuality = "stream/quality";
inline constexpr const char *StreamAudio = "stream/audio";
} // namespace Keys

namespace Defaults {
// General
inline constexpr bool InMemoryCache = false;
inline constexpr bool CloseToTray = false;

// Chat preferences apply to this device.
inline constexpr bool CompactMessages = false;
inline constexpr bool ShowAvatars = true;
inline constexpr bool ShowEmbeds = true;
inline constexpr bool ShowReactions = true;
inline constexpr bool UnderlineLinks = false;
inline constexpr bool ReducedMotion = false;

// Notifications
inline constexpr bool DesktopNotifications = true;
inline constexpr bool FlashTaskbar = true;
inline constexpr bool TaskbarBadge = true;

// Theme
inline constexpr bool ThemeUseSystemColors = false;
inline constexpr bool ThemeFollowDevice = false;
inline constexpr bool ThemeSync = true;

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
