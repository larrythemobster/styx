#include "Notifications.hpp"

#include "Dispatcher.hpp"
#include "Native.hpp"

#include <gio/gio.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Styx::Platform::Notifications {
namespace {

constexpr const char *SERVICE = "org.freedesktop.Notifications";
constexpr const char *OBJECT_PATH = "/org/freedesktop/Notifications";
constexpr const char *APP_ID = "io.github.larrythemobster.styx";
constexpr int CALL_TIMEOUT_MS = 2000;

using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;

// The notification daemon reports clicks as D-Bus signals, which GDBus delivers on the main
// context that was current when the subscription was made. Styx's own loop isn't a GLib one,
// so the subscription lives on a context of its own, run by a thread.
class Backend
{
public:
    static Backend &instance()
    {
        // Never destroyed: its thread can deliver a click while other statics are torn down.
        static Backend &backend = *new Backend;
        return backend;
    }

    bool connect();
    bool show(const Notification &notification, ClickHandler onClick);

private:
    Backend() = default;
    void onSignal(const char *signal, GVariant *parameters);

    std::mutex mutex;
    GDBusConnection *connection = nullptr;
    GMainContext *context = nullptr;
    GMainLoop *loop = nullptr;
    std::thread thread;
    bool connected = false;
    bool markup = false;
    std::unordered_map<std::uint32_t, ClickHandler> handlers;
};

bool Backend::connect()
{
    std::lock_guard lock(mutex);
    if (connected)
        return true;

    if (!connection)
        connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!connection)
        return false;

    Variant capabilities(
        g_dbus_connection_call_sync(connection, SERVICE, OBJECT_PATH, SERVICE,
                                    "GetCapabilities", nullptr, G_VARIANT_TYPE("(as)"),
                                    G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, nullptr, nullptr),
        g_variant_unref);
    if (!capabilities)
        return false;

    GVariantIter *iter = nullptr;
    g_variant_get(capabilities.get(), "(as)", &iter);
    const char *capability = nullptr;
    while (g_variant_iter_loop(iter, "&s", &capability))
        if (g_strcmp0(capability, "body-markup") == 0)
            markup = true;
    g_variant_iter_free(iter);

    context = g_main_context_new();
    loop = g_main_loop_new(context, FALSE);

    g_main_context_push_thread_default(context);
    g_dbus_connection_signal_subscribe(connection, SERVICE, SERVICE, nullptr, OBJECT_PATH, nullptr,
                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                       [](GDBusConnection *, const char *, const char *, const char *,
                                          const char *signal, GVariant *parameters, gpointer self) {
                                           static_cast<Backend *>(self)->onSignal(signal, parameters);
                                       },
                                       this, nullptr);
    g_main_context_pop_thread_default(context);

    thread = std::thread([this] {
        g_main_context_push_thread_default(context);
        g_main_loop_run(loop);
        g_main_context_pop_thread_default(context);
    });

    connected = true;
    return true;
}

void Backend::onSignal(const char *signal, GVariant *parameters)
{
    std::uint32_t id = 0;
    const char *action = nullptr;
    if (g_strcmp0(signal, "ActionInvoked") == 0 && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)"))) {
        g_variant_get(parameters, "(u&s)", &id, &action);
    } else if (g_strcmp0(signal, "NotificationClosed") == 0 && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uu)"))) {
        g_variant_get(parameters, "(uu)", &id, nullptr);
    } else {
        return;
    }

    ClickHandler handler;
    {
        std::lock_guard lock(mutex);
        auto it = handlers.find(id);
        if (it == handlers.end())
            return;
        handler = std::move(it->second);
        handlers.erase(it);
    }

    if (action && g_strcmp0(action, "default") == 0 && handler)
        Dispatcher::main().post(std::move(handler));
}

bool Backend::show(const Notification &notification, ClickHandler onClick)
{
    if (!connect())
        return false;

    const std::string image = notification.image.empty() ? std::string() : pathUtf8(notification.image);

    bool useMarkup = false;
    {
        std::lock_guard lock(mutex);
        useMarkup = markup;
    }

    GVariantBuilder actions;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&actions, "s", "default");
    g_variant_builder_add(&actions, "s", "Open");

    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(APP_ID));
    if (!notification.image.empty())
        g_variant_builder_add(&hints, "{sv}", "image-path", g_variant_new_string(image.c_str()));

    const std::string body = useMarkup ? htmlEscape(notification.body) : notification.body;

    Variant reply(g_dbus_connection_call_sync(connection, SERVICE, OBJECT_PATH, SERVICE, "Notify",
                                              g_variant_new("(susssasa{sv}i)", "Styx", 0u, APP_ID,
                                                            notification.title.c_str(), body.c_str(),
                                                            &actions, &hints, -1),
                                              G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE,
                                              CALL_TIMEOUT_MS, nullptr, nullptr),
                  g_variant_unref);
    if (!reply)
        return false;

    std::uint32_t id = 0;
    g_variant_get(reply.get(), "(u)", &id);
    if (onClick) {
        std::lock_guard lock(mutex);
        handlers[id] = std::move(onClick);
    }
    return true;
}

} // namespace

bool available()
{
    return Backend::instance().connect();
}

bool show(const Notification &notification, ClickHandler onClick)
{
    return Backend::instance().show(notification, std::move(onClick));
}

} // namespace Styx::Platform::Notifications
