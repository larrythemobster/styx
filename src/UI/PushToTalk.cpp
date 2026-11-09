#include "UI/PushToTalk.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/Session.hpp"
#include "Core/SettingsKeys.hpp"
#include "UI/Rml/Shell.hpp"

#ifndef STYX_NO_VOICE
#include "Core/Audio/VoiceManager.hpp"
#endif

namespace Styx::UI {

PushToTalk::PushToTalk(Shell &shell) : shell(shell)
{
    connections.push_back(shell.keyChanged.connect([this](int key, int mods, bool down) {
        onKey(key, mods, down);
    }));
    connections.push_back(shell.focusLost.connect([this] {
        if (!globalHook.active())
            setHeld(false);
    }));
    reload();
}

PushToTalk::~PushToTalk()
{
    globalHook.stop();
}

void PushToTalk::reload()
{
    namespace Settings = Core::Settings;
    enabled = Settings::getString(Settings::Keys::VoiceInputMode, Settings::Defaults::VoiceInputMode) == "push_to_talk";
    binding = Rml::keyBindingFromJson(Settings::get(Settings::Keys::VoicePushToTalkKey));
    updateGlobalHook();
}

void PushToTalk::setSuspended(bool suspendedState)
{
    suspended = suspendedState;
    updateGlobalHook();
}

void PushToTalk::updateGlobalHook()
{
    setHeld(false);
    globalHook.stop();
#if defined(_WIN32) && !defined(STYX_NO_VOICE)
    if (enabled && binding.valid() && !suspended)
        globalHook.start(binding, [this](bool down) { setHeld(down); });
#endif
}

std::string PushToTalk::shortcutScopeDescription() const
{
    if (suspended)
        return "Press a key or key combination to record a shortcut.";
    if (globalHook.active())
        return "Push to talk works even when Styx is not the focused window.";
#ifdef _WIN32
    if (enabled && binding.valid() && !globalHook.error().empty())
        return "Global push to talk is unavailable: " + globalHook.error() + ". Push to talk works only while Styx is focused.";
#endif
    return "Push to talk works while Styx is the focused window.";
}

void PushToTalk::onKey(int key, int rmlModifiers, bool down)
{
    if (globalHook.active() || !enabled || suspended)
        return;
    if (down && Rml::keyBindingPressed(binding, key, rmlModifiers))
        setHeld(true);
    else if (!down && key == binding.key)
        setHeld(false);
}

void PushToTalk::setHeld(bool heldState)
{
    if (heldState == held)
        return;
    held = heldState;
#ifndef STYX_NO_VOICE
    for (const auto &client : shell.session()->getClients()) {
        if (client && client->voice())
            client->voice()->setPushToTalkHeld(heldState);
    }
#endif
}

} // namespace Styx::UI
