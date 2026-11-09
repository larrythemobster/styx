#pragma once

#include "UI/Chat/SelectCandidates.hpp"
#include "UI/Chat/SlashCommands.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Rml { class Element; }

namespace Styx::UI::Rml {

// The inputs shown above the composer for the options of a chosen slash command: a text
// field for strings and numbers, a drop-down for choices, true/false, and the users,
// roles or channels on offer. It keeps what was typed across re-renders; the chat
// controller checks and sends the values.
class CommandOptionsBar
{
public:
    // The element the bar draws into; null hides it.
    void setHost(::Rml::Element *host);

    // Shows the command's options. `candidates[i]` lists the picks for option i when it is
    // a user, role, mentionable or channel option (it may be shorter than the options).
    void show(const SlashCommandEntry &command, std::vector<std::vector<SelectCandidate>> candidates);
    void hide();
    [[nodiscard]] bool isOpen() const { return open; }

    // What each option's control holds, by option index: typed text, the chosen choice's
    // name, "true"/"false", or a candidate's value. Empty when left blank.
    [[nodiscard]] std::vector<std::string> values();

    // An error under each option (empty for none) and one for the whole command.
    void setErrors(std::vector<std::string> optionErrors, std::string commandError);
    // While the command is on its way the inputs are disabled.
    void setPending(bool pending);

    // The bot's autocomplete suggestions for one option, listed under its text field as
    // rows with data-action="command-suggestion", data-option="{index}" and
    // data-choice="{k}"; an empty list hides them. Ignored for an index without a field.
    void setSuggestions(std::size_t optionIndex, std::vector<Discord::ApplicationCommandChoice> choices);
    // Puts suggestion k's value in option optionIndex's field (strings as is, numbers as
    // JSON text) and hides the list. Returns false when there's no such suggestion.
    bool chooseSuggestion(std::size_t optionIndex, std::size_t choice);

private:
    void readDrafts();
    void render();

    ::Rml::Element *host = nullptr;
    bool open = false;
    bool pending = false;
    SlashCommandEntry command;
    std::vector<std::vector<SelectCandidate>> candidates;
    std::vector<std::string> drafts;
    std::vector<std::string> optionErrors;
    std::string commandError;
    std::size_t suggestionOption = 0;
    std::vector<Discord::ApplicationCommandChoice> suggestions;
};

} // namespace Styx::UI::Rml
