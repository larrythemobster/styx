#include "Badge.hpp"

#include <gio/gio.h>

namespace Styx::Platform::Badge {
namespace {

constexpr const char *OBJECT_PATH = "/io/github/larrythemobster/styx/launcher";
constexpr const char *INTERFACE = "com.canonical.Unity.LauncherEntry";
// Docks match this against Styx's installed .desktop file.
constexpr const char *APP_URI = "application://io.github.larrythemobster.styx.desktop";

} // namespace

bool supported() { return true; }

// KDE Plasma, Ubuntu's dock and Dash to Dock show this count on the launcher.
void setCount(void *, int count)
{
    static GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!connection)
        return;
    const gint64 shown = count > 0 ? count : 0;
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", "count", g_variant_new_int64(shown));
    g_variant_builder_add(&properties, "{sv}", "count-visible", g_variant_new_boolean(shown > 0));
    g_dbus_connection_emit_signal(connection, nullptr, OBJECT_PATH, INTERFACE, "Update",
                                  g_variant_new("(sa{sv})", APP_URI, &properties), nullptr);
    g_dbus_connection_flush_sync(connection, nullptr, nullptr);
}

} // namespace Styx::Platform::Badge
