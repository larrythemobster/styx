#pragma once

#include <QSettings>
#include <QString>
#include <QVariant>

namespace Styx::Core::Settings {

namespace Keys {
// General / System
inline constexpr const char *InMemoryCache = "general/in_memory_cache";
inline constexpr const char *CloseToTray = "general/close_to_tray";
inline constexpr const char *FontEngine = "general/font_engine";

// Chat / Media
inline constexpr const char *AnimateEmoji = "chat/animate_emoji";
inline constexpr const char *AnimateStickers = "chat/animate_stickers";
inline constexpr const char *GifPlayback = "chat/gif_playback";
inline constexpr const char *GifMaxPlaying = "chat/gif_max_playing";
inline constexpr const char *GifMaxMb = "chat/gif_max_mb";
inline constexpr const char *AnimationCacheMb = "chat/animation_cache_mb";
inline constexpr const char *AnimationMaxMb = "chat/animation_max_mb";

// Theme / Appearance
inline constexpr const char *ThemeUseSystemColors = "theme/use_system_colors";
inline constexpr const char *ThemeUseSystemStyle = "theme/use_system_style";

// Audio & Voice
inline constexpr const char *AudioBackend = "audio/backend";
inline constexpr const char *VoiceInputDevice = "voice/input_device";
inline constexpr const char *VoiceOutputDevice = "voice/output_device";

// Stream / Go Live
inline constexpr const char *StreamQuality = "stream/quality";
inline constexpr const char *StreamAudio = "stream/audio";
} // namespace Keys

namespace Defaults {
// General
inline constexpr bool InMemoryCache = false;
inline constexpr bool CloseToTray = false;
inline constexpr const char *FontEngine = "directwrite";

// Chat
inline constexpr bool AnimateEmoji = true;
inline constexpr bool AnimateStickers = true;
inline constexpr const char *GifPlayback = "focused";
inline constexpr int GifMaxPlaying = 4;
inline constexpr int GifMaxMb = 32;
inline constexpr int AnimationCacheMb = 128;
inline constexpr int AnimationMaxMb = 16;

// Theme
inline constexpr bool ThemeUseSystemColors = false;
inline constexpr bool ThemeUseSystemStyle = false;

// Audio & Voice
inline constexpr const char *AudioBackend = "";
inline constexpr const char *VoiceInputDevice = "";
inline constexpr const char *VoiceOutputDevice = "";

// Stream
inline constexpr const char *StreamQuality = "720p30";
inline constexpr bool StreamAudio = true;
} // namespace Defaults

// Storage wrapper: reads and writes persistent configuration.
// Swapping QSettings to Platform::Settings in the future will be done in this wrapper.
inline QVariant get(const QString &key, const QVariant &defaultValue = {})
{
    return QSettings().value(key, defaultValue);
}

inline bool getBool(const QString &key, bool defaultValue)
{
    return QSettings().value(key, defaultValue).toBool();
}

inline int getInt(const QString &key, int defaultValue)
{
    return QSettings().value(key, defaultValue).toInt();
}

inline QString getString(const QString &key, const QString &defaultValue = {})
{
    return QSettings().value(key, defaultValue).toString();
}

inline void set(const QString &key, const QVariant &value)
{
    QSettings().setValue(key, value);
}

inline void setBool(const QString &key, bool value)
{
    QSettings().setValue(key, value);
}

inline void setInt(const QString &key, int value)
{
    QSettings().setValue(key, value);
}

inline void setString(const QString &key, const QString &value)
{
    QSettings().setValue(key, value);
}

} // namespace Styx::Core::Settings
