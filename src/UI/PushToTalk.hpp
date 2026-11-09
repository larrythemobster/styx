#pragma once

#include "Platform/Signal.hpp"
#include "UI/Rml/Shortcuts.hpp"
#ifdef __linux__
#include "UI/X11PttHook.hpp"
#else
#include "UI/WindowsPttHook.hpp"
#endif

#include <string>
#include <vector>

namespace Styx::UI {

class Shell;

// Holds every account's microphone open while the push-to-talk key is down.
// Supports global input on Windows and X11 even when another application has focus.
// On Wayland, macOS, or on installation failure, push-to-talk is focused-only and
// releases when the window loses focus.
class PushToTalk
{
public:
    explicit PushToTalk(Shell &shell);
    ~PushToTalk();

    // Rereads the input mode and binding from settings; call after changing them.
    void reload();
    // Ignores the key while the settings page records a new binding.
    void setSuspended(bool suspended);
    [[nodiscard]] bool isSuspended() const { return suspended; }
    [[nodiscard]] std::string shortcutScopeDescription() const;

private:
    void onKey(int key, int rmlModifiers, bool down);
    void setHeld(bool held);
    void updateGlobalHook();

    Shell &shell;
    bool enabled = false;
    bool suspended = false;
    bool held = false;
    Rml::KeyBinding binding;
    std::vector<Platform::Connection> connections;
#ifdef __linux__
    X11PttHook globalHook;
#else
    WindowsPttHook globalHook;
#endif
};

} // namespace Styx::UI
