#pragma once

#include <optional>
#include <string>

namespace Styx::Platform {

class SecureStore {
public:
    // Write credentials with service and key.
    // On Windows, the target name matches QtKeychain format: key (or service if key is empty).
    static bool writePassword(const std::string &service, const std::string &key, const std::string &password);
    static std::optional<std::string> readPassword(const std::string &service, const std::string &key);
    static bool deletePassword(const std::string &service, const std::string &key);

    // Overloads using target name directly
    static bool writePassword(const std::string &key, const std::string &password)
    {
        return writePassword("", key, password);
    }
    static std::optional<std::string> readPassword(const std::string &key)
    {
        return readPassword("", key);
    }
    static bool deletePassword(const std::string &key)
    {
        return deletePassword("", key);
    }
};

} // namespace Styx::Platform
