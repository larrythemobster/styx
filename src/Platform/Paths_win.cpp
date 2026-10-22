#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Paths.hpp"

#include <cstdlib>
#include <vector>

namespace Styx::Platform::Paths {

static std::filesystem::path getEnvPath(const wchar_t *var)
{
    wchar_t buffer[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(var, buffer, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buffer);
    }
    if (len >= MAX_PATH) {
        std::vector<wchar_t> longBuf(len + 1);
        DWORD len2 = GetEnvironmentVariableW(var, longBuf.data(), len + 1);
        if (len2 > 0 && len2 <= len) {
            return std::filesystem::path(longBuf.data());
        }
    }
    return {};
}

std::filesystem::path configDir()
{
    std::filesystem::path base = getEnvPath(L"APPDATA");
    if (base.empty()) {
        std::filesystem::path userProfile = getEnvPath(L"USERPROFILE");
        if (!userProfile.empty()) {
            base = userProfile / "AppData" / "Roaming";
        } else {
            base = "C:\\AppData\\Roaming";
        }
    }
    return base / "larrythemobster" / "Styx";
}

std::filesystem::path dataDir()
{
    return configDir();
}

std::filesystem::path cacheDir()
{
    std::filesystem::path base = getEnvPath(L"LOCALAPPDATA");
    if (base.empty()) {
        std::filesystem::path userProfile = getEnvPath(L"USERPROFILE");
        if (!userProfile.empty()) {
            base = userProfile / "AppData" / "Local";
        } else {
            base = "C:\\AppData\\Local";
        }
    }
    return base / "larrythemobster" / "Styx";
}

std::filesystem::path executablePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return {};
        }
        if (size < buffer.size()) {
            return std::filesystem::path(buffer.data());
        }
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace Styx::Platform::Paths
#endif
