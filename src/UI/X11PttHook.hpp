#pragma once

#include "UI/Rml/Shortcuts.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace Styx::UI {

// Observes XInput2 raw keys on its own X11 connection without grabbing them. The worker owns
// that connection; callbacks run on Dispatcher::main. Stop cancels queued callbacks and joins
// the worker. A Wayland video driver must never use an available Xwayland DISPLAY as a bypass.
class X11PttHook
{
public:
    X11PttHook();
    ~X11PttHook();
    X11PttHook(const X11PttHook &) = delete;
    X11PttHook &operator=(const X11PttHook &) = delete;

    bool start(const Rml::KeyBinding &binding, std::function<void(bool)> callback,
               std::string_view videoDriver = "x11");
    void stop();
    [[nodiscard]] bool active() const;
    [[nodiscard]] const std::string &error() const;

    // X11 KeySym, or zero for an unsupported key. Main Enter and keypad Enter are distinct.
    [[nodiscard]] static unsigned long keySym(int rmlKey);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Styx::UI
