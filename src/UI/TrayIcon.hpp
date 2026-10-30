#pragma once
#include "Platform/Trackable.hpp"
#include <string>
struct SDL_Tray;

namespace Styx::UI {
class TrayIcon : public Platform::Trackable {
public:
    TrayIcon();
    ~TrayIcon() override;
    bool available() const { return tray != nullptr; }
    void setToolTip(const std::string &title);
    Platform::Signal<> showWindowRequested;
    Platform::Signal<> quitRequested;
private:
    SDL_Tray *tray = nullptr;
};
}
