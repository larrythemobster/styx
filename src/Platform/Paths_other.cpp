#ifndef _WIN32
#include "Paths.hpp"

#include <cstdlib>
#include <vector>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace Styx::Platform::Paths {

static std::filesystem::path getHomeDir()
{
    const char *home = std::getenv("HOME");
    if (home && *home) {
        return home;
    }
    return "/tmp";
}

#ifdef __APPLE__
std::filesystem::path configDir()
{
    return getHomeDir() / "Library" / "Application Support" / "larrythemobster" / "Styx";
}

std::filesystem::path dataDir()
{
    return configDir();
}

std::filesystem::path cacheDir()
{
    return getHomeDir() / "Library" / "Caches" / "larrythemobster" / "Styx";
}
#else // Linux / BSD / XDG
std::filesystem::path configDir()
{
    const char *xdgConfig = std::getenv("XDG_CONFIG_HOME");
    if (xdgConfig && *xdgConfig) {
        return std::filesystem::path(xdgConfig) / "larrythemobster" / "Styx";
    }
    return getHomeDir() / ".config" / "larrythemobster" / "Styx";
}

std::filesystem::path dataDir()
{
    const char *xdgData = std::getenv("XDG_DATA_HOME");
    if (xdgData && *xdgData) {
        return std::filesystem::path(xdgData) / "larrythemobster" / "Styx";
    }
    return getHomeDir() / ".local" / "share" / "larrythemobster" / "Styx";
}

std::filesystem::path cacheDir()
{
    const char *xdgCache = std::getenv("XDG_CACHE_HOME");
    if (xdgCache && *xdgCache) {
        return std::filesystem::path(xdgCache) / "larrythemobster" / "Styx";
    }
    return getHomeDir() / ".cache" / "larrythemobster" / "Styx";
}
#endif

std::filesystem::path executablePath()
{
    std::error_code ec;
#ifdef __APPLE__
    uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::vector<char> path(length);
    if (_NSGetExecutablePath(path.data(), &length) != 0) return {};
    return std::filesystem::canonical(path.data(), ec);
#else
    return std::filesystem::canonical("/proc/self/exe", ec);
#endif
}

} // namespace Styx::Platform::Paths
#endif
