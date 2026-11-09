#pragma once

#include "Platform/Signal.hpp"
#include "UI/Rml/Shortcuts.hpp"
#include "UI/WindowsPttHook.hpp"

#include <string>
#include <vector>

namespace Styx::UI {

class Shell;

// Holds every account's microphone open while the push-to-talk key is down.
// On Windows, a low-level keyboard hook catches the shortcut globally even when another
// application has focus. On other platforms, or if the hook cannot be installed, push-to-talk
// falls back to focused-window events and releases when the window loses focus.
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
    WindowsPttHook globalHook;
};

} // namespace Styx::UI
