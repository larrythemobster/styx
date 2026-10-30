#include "SecureStore.hpp"

#include <libsecret/secret.h>
#include <gio/gio.h>

#include <memory>

namespace Styx::Platform {
namespace {

const SecretSchema schema = {
    "org.qt.keychain", SECRET_SCHEMA_DONT_MATCH_NAME,
    {
        {"user", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {"server", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {"type", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}
    }
};

bool validText(const std::string &text)
{
    return text.find('\0') == std::string::npos &&
           g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr);
}

using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
using Connection = std::unique_ptr<GDBusConnection, decltype(&g_object_unref)>;

struct WalletEndpoint {
    const char *name;
    const char *path;
};

const WalletEndpoint *walletEndpoint()
{
    static const WalletEndpoint wallet4{"org.kde.kwalletd", "/modules/kwalletd"};
    static const WalletEndpoint wallet5{"org.kde.kwalletd5", "/modules/kwalletd5"};
    static const WalletEndpoint wallet6{"org.kde.kwalletd6", "/modules/kwalletd6"};

    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    const char *session = g_getenv("DESKTOP_SESSION");
    const char *fullSession = g_getenv("KDE_FULL_SESSION");
    const bool kde = (desktop && (g_strrstr(desktop, "KDE") || g_strrstr(desktop, "PLASMA"))) ||
                     (session && (g_strrstr(session, "kde") || g_strrstr(session, "plasma"))) ||
                     (fullSession && *fullSession);
    if (!kde)
        return nullptr;

    const char *version = g_getenv("KDE_SESSION_VERSION");
    if (g_strcmp0(version, "6") == 0) return &wallet6;
    if (g_strcmp0(version, "5") == 0) return &wallet5;
    if (g_strcmp0(version, "4") == 0 || g_strcmp0(session, "kde4") == 0) return &wallet4;
    return nullptr;
}

Variant walletCall(GDBusConnection *connection, const WalletEndpoint &endpoint,
                   const char *interface, const char *method, GVariant *parameters,
                   const GVariantType *replyType, int timeout = -1)
{
    return Variant(g_dbus_connection_call_sync(connection, endpoint.name, endpoint.path,
                                               interface, method, parameters, replyType,
                                               G_DBUS_CALL_FLAGS_NONE, timeout, nullptr, nullptr),
                   g_variant_unref);
}

struct Wallet {
    Connection connection{nullptr, g_object_unref};
    const WalletEndpoint *endpoint = nullptr;
    int handle = -1;

    explicit Wallet(const std::string &service)
    {
        endpoint = walletEndpoint();
        if (!endpoint) return;
        connection.reset(g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr));
        if (!connection) return;
        GError *error = nullptr;
        Variant ping(g_dbus_connection_call_sync(connection.get(), endpoint->name, endpoint->path,
                                                 "org.freedesktop.DBus.Peer", "Ping", nullptr,
                                                 G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NONE,
                                                 -1, nullptr, &error), g_variant_unref);
        if (!ping) {
            if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
                g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER))
                endpoint = nullptr;
            if (error) g_error_free(error);
            return;
        }

        auto name = walletCall(connection.get(), *endpoint, "org.kde.KWallet",
                               "networkWallet", nullptr, G_VARIANT_TYPE("(s)"));
        if (!name) return;
        const char *walletName = nullptr;
        g_variant_get(name.get(), "(&s)", &walletName);
        auto opened = walletCall(connection.get(), *endpoint, "org.kde.KWallet", "open",
                                 g_variant_new("(sxs)", walletName, static_cast<gint64>(0),
                                               service.c_str()),
                                 G_VARIANT_TYPE("(i)"), G_MAXINT);
        if (opened) g_variant_get(opened.get(), "(i)", &handle);
    }

    Variant call(const char *method, GVariant *parameters, const GVariantType *replyType)
    {
        return walletCall(connection.get(), *endpoint, "org.kde.KWallet", method,
                          parameters, replyType, G_MAXINT);
    }
};

GVariant *entryArguments(int handle, const std::string &service, const std::string &key)
{
    return g_variant_new("(isss)", handle, service.c_str(), key.c_str(), service.c_str());
}

} // namespace

bool SecureStore::writePassword(const std::string &service, const std::string &key, const std::string &password)
{
    if (!validText(service) || !validText(key) || !validText(password))
        return false;

    Wallet wallet(service);
    if (wallet.endpoint) {
        if (wallet.handle < 0) return false;
        auto folder = wallet.call("hasFolder",
                                  g_variant_new("(iss)", wallet.handle, service.c_str(), service.c_str()),
                                  G_VARIANT_TYPE("(b)"));
        if (!folder) return false;
        gboolean exists = FALSE;
        g_variant_get(folder.get(), "(b)", &exists);
        if (!exists) {
            auto created = wallet.call("createFolder",
                                       g_variant_new("(iss)", wallet.handle, service.c_str(), service.c_str()),
                                       G_VARIANT_TYPE("(b)"));
            if (!created) return false;
            g_variant_get(created.get(), "(b)", &exists);
            if (!exists) return false;
        }
        auto result = wallet.call("writePassword",
                                  g_variant_new("(issss)", wallet.handle, service.c_str(), key.c_str(),
                                                password.c_str(), service.c_str()),
                                  G_VARIANT_TYPE("(i)"));
        if (!result) return false;
        gint status = -1;
        g_variant_get(result.get(), "(i)", &status);
        return status == 0;
    }

    const std::string label = key.empty() ? service : (service.empty() ? key : key + "@" + service);
    GError *error = nullptr;
    const gboolean stored = secret_password_store_sync(
        &schema, SECRET_COLLECTION_DEFAULT, label.c_str(), password.c_str(), nullptr, &error,
        "user", key.c_str(), "server", service.c_str(), "type", "plaintext", nullptr);
    if (error) {
        g_error_free(error);
        return false;
    }
    return stored != FALSE;
}

std::optional<std::string> SecureStore::readPassword(const std::string &service, const std::string &key)
{
    if (!validText(service) || !validText(key))
        return std::nullopt;

    Wallet wallet(service);
    if (wallet.endpoint) {
        if (wallet.handle < 0) return std::nullopt;
        auto type = wallet.call("entryType", entryArguments(wallet.handle, service, key),
                                G_VARIANT_TYPE("(i)"));
        if (!type) return std::nullopt;
        gint entryType = 0;
        g_variant_get(type.get(), "(i)", &entryType);
        if (entryType == 1) {
            auto result = wallet.call("readPassword", entryArguments(wallet.handle, service, key),
                                      G_VARIANT_TYPE("(s)"));
            if (!result) return std::nullopt;
            const char *value = nullptr;
            g_variant_get(result.get(), "(&s)", &value);
            return std::string(value);
        }
        if (entryType == 2) {
            auto result = wallet.call("readEntry", entryArguments(wallet.handle, service, key),
                                      G_VARIANT_TYPE("(ay)"));
            if (!result) return std::nullopt;
            Variant bytes(g_variant_get_child_value(result.get(), 0), g_variant_unref);
            gsize size = 0;
            const auto *data = static_cast<const char *>(g_variant_get_fixed_array(bytes.get(), &size, 1));
            return size ? std::string(data, size) : std::string{};
        }
        return std::nullopt;
    }

    GError *error = nullptr;
    std::unique_ptr<gchar, decltype(&secret_password_free)> password(secret_password_lookup_sync(
        &schema, nullptr, &error,
        "user", key.c_str(), "server", service.c_str(), "type", "plaintext", nullptr), secret_password_free);
    if (error) {
        g_error_free(error);
        return std::nullopt;
    }
    if (password)
        return std::string(password.get());

    password.reset(secret_password_lookup_sync(
        &schema, nullptr, &error,
        "user", key.c_str(), "server", service.c_str(), "type", "base64", nullptr));
    if (error) {
        g_error_free(error);
        return std::nullopt;
    }
    if (!password)
        return std::nullopt;

    gsize size = 0;
    std::unique_ptr<guchar, decltype(&g_free)> decoded(g_base64_decode(password.get(), &size), g_free);
    if (size == 0) return std::string{};
    std::string result(reinterpret_cast<const char *>(decoded.get()), size);
    volatile guchar *bytes = decoded.get();
    for (gsize i = 0; i < size; ++i)
        bytes[i] = 0;
    return result;
}

bool SecureStore::deletePassword(const std::string &service, const std::string &key)
{
    if (!validText(service) || !validText(key))
        return false;

    Wallet wallet(service);
    if (wallet.endpoint) {
        if (wallet.handle < 0) return false;
        auto result = wallet.call("removeEntry", entryArguments(wallet.handle, service, key),
                                  G_VARIANT_TYPE("(i)"));
        if (!result) return false;
        gint status = -1;
        g_variant_get(result.get(), "(i)", &status);
        return status == 0;
    }

    GError *error = nullptr;
    secret_password_clear_sync(
        &schema, nullptr, &error,
        "user", key.c_str(), "server", service.c_str(), nullptr);
    if (error) {
        g_error_free(error);
        return false;
    }
    return true;
}

} // namespace Styx::Platform
