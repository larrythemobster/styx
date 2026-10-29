#include "TokenStore.hpp"

#include "Platform/SecureStore.hpp"
#include "Logging.hpp"

namespace Styx::Core {

std::string TokenStore::keyForAccount(Snowflake accountId)
{
    return "account_" + accountId.toString() + "_token";
}

std::string TokenStore::describeKeychainFailure(const std::string &keychainError)
{
    std::string description = "The system keychain reported: " + keychainError;
#if !defined(_WIN32) && !defined(__APPLE__)
    description += "\n\nStyx keeps tokens in the system keyring, which needs a Secret Service provider such as GNOME Keyring, KWallet or KeePassXC to be installed and running.";
#endif
    return description;
}

Result<void> TokenStore::saveToken(Snowflake accountId, const std::string &token)
{
    if (!Platform::SecureStore::writePassword(SERVICE_NAME, keyForAccount(accountId), token)) {
        Core::log(Core::LogLevel::Warning, LogCore) << "TokenStore: Failed to save token for account" << accountId.toString();
        return Result<void>::makeError(describeKeychainFailure("Failed to write credential to secure store"));
    }
    return Result<void>::makeOk();
}

Result<std::string> TokenStore::loadToken(Snowflake accountId)
{
    auto token = Platform::SecureStore::readPassword(SERVICE_NAME, keyForAccount(accountId));
    if (!token || token->empty())
        return Result<std::string>::makeError("No token is stored for this account. Use \"Set Token\" to enter it again.");
    return Result<std::string>::makeOk(*token);
}

bool TokenStore::deleteToken(Snowflake accountId)
{
    return Platform::SecureStore::deletePassword(SERVICE_NAME, keyForAccount(accountId));
}

} // namespace Styx::Core
