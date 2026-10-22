#ifndef _WIN32
#include "SecureStore.hpp"

#include <mutex>
#include <unordered_map>

namespace Styx::Platform {

// Clean in-memory fallback for other OSes until libsecret / keychain_apple are integrated
static std::mutex s_storeMutex;
static std::unordered_map<std::string, std::string> s_fallbackStore;

static std::string makeKey(const std::string &service, const std::string &key)
{
    return service.empty() ? key : (service + "/" + key);
}

bool SecureStore::writePassword(const std::string &service, const std::string &key, const std::string &password)
{
    std::lock_guard<std::mutex> lock(s_storeMutex);
    s_fallbackStore[makeKey(service, key)] = password;
    return true;
}

std::optional<std::string> SecureStore::readPassword(const std::string &service, const std::string &key)
{
    std::lock_guard<std::mutex> lock(s_storeMutex);
    auto it = s_fallbackStore.find(makeKey(service, key));
    if (it != s_fallbackStore.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool SecureStore::deletePassword(const std::string &service, const std::string &key)
{
    std::lock_guard<std::mutex> lock(s_storeMutex);
    s_fallbackStore.erase(makeKey(service, key));
    return true;
}

} // namespace Styx::Platform
#endif
