// Ported from ouwou upstream/video-streams (71b4907); original implementation retained.
#pragma once
extern "C" {
#include <libavutil/hwcontext.h>
}
#if defined(__unix__) && !defined(__APPLE__)
#include <dlfcn.h>
#include <initializer_list>
#endif
namespace Styx::Core::Media {
[[nodiscard]] inline bool hardwareDeviceCanBeAskedFor(AVHWDeviceType type)
{
#if defined(__unix__) && !defined(__APPLE__)
    if (type != AV_HWDEVICE_TYPE_VAAPI) return true;
    static const bool loadable = [] {
        if (void *linked = dlopen("libva.so.2", RTLD_NOLOAD | RTLD_LAZY)) { dlclose(linked); return true; }
        for (const char *library : { "libva.so.2", "libva-drm.so.2", "libva-x11.so.2" }) if (!dlopen(library, RTLD_LAZY | RTLD_GLOBAL)) return false;
        return true;
    }();
    return loadable;
#else
    (void)type; return true;
#endif
}
} // namespace Styx::Core::Media
