#include "TokenStore.hpp"

#include "Platform/SecureStore.hpp"
#include "Logging.hpp"

namespace Styx {
namespace Core {

QString TokenStore::keyForAccount(Snowflake accountId)
{
    return QString("account_%1_token").arg(accountId.toString());
}

QString TokenStore::describeKeychainFailure(const QString &keychainError)
{
    QString description = tr("The system keychain reported: %1").arg(keychainError);
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    description += "\n\n" + tr("Styx keeps tokens in the system keyring, which needs a Secret Service provider such as GNOME Keyring, KWallet or KeePassXC to be installed and running.");
#endif
    return description;
}

Result<void> TokenStore::saveToken(Snowflake accountId, const QString &token)
{
    std::string key = keyForAccount(accountId).toStdString();
    std::string pass = token.toStdString();

    if (!Platform::SecureStore::writePassword(SERVICE_NAME, key, pass)) {
        qCWarning(LogCore) << "TokenStore: Failed to save token for account"
                           << accountId;
        return Result<void>::makeError(describeKeychainFailure(tr("Failed to write credential to secure store")));
    }

    return Result<void>::makeOk();
}

Result<QString> TokenStore::loadToken(Snowflake accountId)
{
    std::string key = keyForAccount(accountId).toStdString();
    auto opt = Platform::SecureStore::readPassword(SERVICE_NAME, key);

    if (!opt.has_value() || opt->empty()) {
        return Result<QString>::makeError(tr("No token is stored for this account. Use \"Set Token\" to enter it again."));
    }

    return Result<QString>::makeOk(QString::fromStdString(*opt));
}

bool TokenStore::deleteToken(Snowflake accountId)
{
    std::string key = keyForAccount(accountId).toStdString();
    return Platform::SecureStore::deletePassword(SERVICE_NAME, key);
}

} // namespace Core
} // namespace Styx
