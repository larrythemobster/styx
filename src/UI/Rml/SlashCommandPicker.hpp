#pragma once

#include "UI/Chat/SlashCommands.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace Rml { class Element; }

namespace Styx::UI::Rml {

// The list that opens above the composer while a "/command" is being typed. It only
// draws and tracks the highlighted row; the chat controller decides what choosing means.
class SlashCommandPicker
{
public:
    // The element the list draws into; null hides it.
    void setHost(::Rml::Element *host);

    // Shows the commands matching `query`, or a loading line while the indexes are on
    // their way and nothing is known yet.
    void show(std::string_view query, std::vector<SlashCommandEntry> commands, bool loading);
    void hide();
    [[nodiscard]] bool isOpen() const { return open; }

    enum class Key { None, Up, Down, Choose, Close };
    // Moves the highlight; Choose returns the highlighted command, if any.
    const SlashCommandEntry *press(Key key);
    // The command a clicked row stands for.
    [[nodiscard]] const SlashCommandEntry *entryAt(std::size_t row) const;

private:
    void render();

    ::Rml::Element *host = nullptr;
    bool open = false;
    bool loading = false;
    std::vector<SlashCommandEntry> commands;
    std::vector<std::size_t> matches;
    std::size_t active = 0;
};

} // namespace Styx::UI::Rml
