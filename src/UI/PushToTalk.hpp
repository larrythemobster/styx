#pragma once

#include "Platform/Signal.hpp"
#include "UI/Rml/Shortcuts.hpp"

#include <vector>

namespace Styx::UI {

class Shell;

// Holds every account's microphone open while the push-to-talk key is down in a Styx window.
// Owned by Shell. Releases on key up and when the window loses focus, so a key let go
// elsewhere can't leave the mic open.
class PushToTalk
{
public:
    explicit PushToTalk(Shell &shell);

    // Rereads the input mode and binding from settings; call after changing them.
    void reload();
    // Ignores the key while the settings page records a new binding.
    void setSuspended(bool suspended);
    [[nodiscard]] bool isSuspended() const { return suspended; }

private:
    void onKey(int key, int rmlModifiers, bool down);
    void setHeld(bool held);

    Shell &shell;
    bool enabled = false;
    bool suspended = false;
    bool held = false;
    Rml::KeyBinding binding;
    std::vector<Platform::Connection> connections;
};

} // namespace Styx::UI
