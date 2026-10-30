#include "TrayIcon.hpp"
#include "Core/Logging.hpp"
#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include <SDL3/SDL.h>

namespace Styx::UI {
TrayIcon::TrayIcon()
{
    auto image = Platform::Image::decodeFile(Platform::Resources::path("app/styx.svg"));
    SDL_Surface *surface = image ? SDL_CreateSurfaceFrom(image->width, image->height, SDL_PIXELFORMAT_RGBA32,
        image->pixels.data(), static_cast<int>(image->stride())) : nullptr;
    tray = SDL_CreateTray(surface, "Styx");
    if (surface) SDL_DestroySurface(surface);
    if (!tray) { Core::log(Core::LogLevel::Warning, LogUI) << "System tray unavailable:" << SDL_GetError(); return; }
    auto *menu = SDL_CreateTrayMenu(tray);
    if (!menu) { SDL_DestroyTray(tray); tray = nullptr; return; }
    auto *show = SDL_InsertTrayEntryAt(menu, -1, "Show Styx", SDL_TRAYENTRY_BUTTON);
    SDL_InsertTrayEntryAt(menu, -1, nullptr, SDL_TRAYENTRY_BUTTON);
    auto *quit = SDL_InsertTrayEntryAt(menu, -1, "Quit", SDL_TRAYENTRY_BUTTON);
    if (!show || !quit) { SDL_DestroyTray(tray); tray = nullptr; return; }
    SDL_SetTrayEntryCallback(show, [](void *data, SDL_TrayEntry *) { static_cast<TrayIcon *>(data)->showWindowRequested.publish(); }, this);
    SDL_SetTrayEntryCallback(quit, [](void *data, SDL_TrayEntry *) { static_cast<TrayIcon *>(data)->quitRequested.publish(); }, this);
}
TrayIcon::~TrayIcon() { if (tray) SDL_DestroyTray(tray); }
void TrayIcon::setToolTip(const std::string &title) { if (tray) SDL_SetTrayTooltip(tray, title.empty() ? "Styx" : title.c_str()); }
}
