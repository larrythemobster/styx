#pragma once

#include "UI/Rml/Shortcuts.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Styx::UI {

// Converts Windows virtual keys into held/released transitions for one binding. Modifiers use
// ShortcutModifier bits. Kept separate from the OS hook so repeats and releases can be tested.
class WindowsPttKeyState
{
public:
    WindowsPttKeyState(int virtualKey, int modifiers);
    std::optional<bool> update(int virtualKey, int modifiers, bool down);

private:
    int targetKey;
    int requiredModifiers;
    bool held = false;
};

// Passive WH_KEYBOARD_LL observer with its own message thread. start/stop and destruction run
// on the main thread. Only binding transitions are posted to Dispatcher::main; stop invalidates
// queued callbacks before joining the thread. No observed key is consumed.
class WindowsPttHook
{
public:
    WindowsPttHook();
    ~WindowsPttHook();
    WindowsPttHook(const WindowsPttHook &) = delete;
    WindowsPttHook &operator=(const WindowsPttHook &) = delete;

    bool start(const Rml::KeyBinding &binding, std::function<void(bool)> callback);
    void stop();
    [[nodiscard]] bool active() const;
    [[nodiscard]] const std::string &error() const;

    // Zero for unsupported keys. The left/right variants of modifier keys remain distinct.
    [[nodiscard]] static int virtualKey(int rmlKey);
    [[nodiscard]] static int normalizeVirtualKey(int virtualKey, int scanCode, bool extended);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Styx::UI
