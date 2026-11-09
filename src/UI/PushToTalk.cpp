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
        setHeld(false);
    }));
    reload();
}

void PushToTalk::reload()
{
    namespace Settings = Core::Settings;
    enabled = Settings::getString(Settings::Keys::VoiceInputMode, Settings::Defaults::VoiceInputMode) == "push_to_talk";
    binding = Rml::keyBindingFromJson(Settings::get(Settings::Keys::VoicePushToTalkKey));
    if (!enabled || !binding.valid())
        setHeld(false);
}

void PushToTalk::setSuspended(bool suspendedState)
{
    suspended = suspendedState;
    if (suspendedState)
        setHeld(false);
}

void PushToTalk::onKey(int key, int rmlModifiers, bool down)
{
    if (!enabled || suspended)
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
