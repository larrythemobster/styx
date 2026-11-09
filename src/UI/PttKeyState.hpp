#pragma once

#include <optional>

namespace Styx::UI {

// One binding's native key identity and ShortcutModifier bits. Repeated presses and unrelated
// keys do not change transmission, and releasing the target works after modifiers are let go.
class PttKeyState
{
public:
    PttKeyState(int key, int modifiers) : targetKey(key), requiredModifiers(modifiers) {}

    std::optional<bool> update(int key, int modifiers, bool down)
    {
        if (targetKey == 0 || key != targetKey)
            return std::nullopt;
        if (down) {
            if (!held && (modifiers & requiredModifiers) == requiredModifiers) {
                held = true;
                return true;
            }
        } else if (held) {
            held = false;
            return false;
        }
        return std::nullopt;
    }

private:
    int targetKey;
    int requiredModifiers;
    bool held = false;
};

} // namespace Styx::UI
