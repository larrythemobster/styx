#pragma once

#include <filesystem>

namespace Styx::Platform::Paths {

// Returns the platform-specific application config directory.
// Windows: %APPDATA%\larrythemobster\Styx
// Linux:   $XDG_CONFIG_HOME/larrythemobster/Styx (~/.config/larrythemobster/Styx)
// macOS:   ~/Library/Application Support/larrythemobster/Styx
std::filesystem::path configDir();

// Returns the platform-specific persistent data directory.
// Windows: %APPDATA%\larrythemobster\Styx
// Linux:   $XDG_DATA_HOME/larrythemobster/Styx (~/.local/share/larrythemobster/Styx)
// macOS:   ~/Library/Application Support/larrythemobster/Styx
std::filesystem::path dataDir();

// Returns the platform-specific cache directory.
// Windows: %LOCALAPPDATA%\larrythemobster\Styx
// Linux:   $XDG_CACHE_HOME/larrythemobster/Styx (~/.cache/larrythemobster/Styx)
// macOS:   ~/Library/Caches/larrythemobster/Styx
std::filesystem::path cacheDir();

// Returns the full path of the current executable.
std::filesystem::path executablePath();

// Ensures that the specified directory exists on disk, creating intermediate folders if necessary.
void ensureDirExists(const std::filesystem::path &path);

} // namespace Styx::Platform::Paths
