#include "SlashCommandPicker.hpp"

#include "Platform/Native.hpp"

#include <RmlUi/Core/Element.h>

#include <format>
#include <string>
#include <utility>

namespace Styx::UI::Rml {

namespace {

// Show at most this many rows; typing narrows the rest.
constexpr std::size_t MaxShownCommands = 25;

} // namespace

void SlashCommandPicker::setHost(::Rml::Element *element)
{
    host = element;
    render();
}

void SlashCommandPicker::show(std::string_view query, std::vector<SlashCommandEntry> available, bool isLoading)
{
    commands = std::move(available);
    matches = matchSlashCommands(commands, query, MaxShownCommands);
    loading = isLoading && commands.empty();
    if (active >= matches.size())
        active = 0;
    open = true;
    render();
}

void SlashCommandPicker::hide()
{
    if (!open)
        return;
    open = false;
    active = 0;
    commands.clear();
    matches.clear();
    render();
}

const SlashCommandEntry *SlashCommandPicker::press(Key key)
{
    if (!open || matches.empty())
        return nullptr;
    switch (key) {
    case Key::Up:
        active = active == 0 ? matches.size() - 1 : active - 1;
        render();
        return nullptr;
    case Key::Down:
        active = (active + 1) % matches.size();
        render();
        return nullptr;
    case Key::Choose:
        return entryAt(active);
    case Key::Close:
        hide();
        return nullptr;
    case Key::None:
        return nullptr;
    }
    return nullptr;
}

const SlashCommandEntry *SlashCommandPicker::entryAt(std::size_t row) const
{
    return open && row < matches.size() ? &commands[matches[row]] : nullptr;
}

void SlashCommandPicker::render()
{
    if (!host)
        return;
    if (!open) {
        host->SetProperty("display", "none");
        host->SetInnerRML("");
        return;
    }

    std::string rml = "<div class=\"command-picker-title\">Commands</div>";
    if (loading) {
        rml += "<div class=\"command-picker-empty\">Loading commands…</div>";
    } else if (matches.empty()) {
        rml += "<div class=\"command-picker-empty\">No commands match</div>";
    } else {
        for (std::size_t row = 0; row < matches.size(); ++row) {
            const auto &entry = commands[matches[row]];
            rml += std::format(
                "<div class=\"command-picker-row{0}\" data-action=\"command-picker-choose\" data-index=\"{1}\">"
                "<span class=\"command-picker-name\">/{2}</span>"
                "<span class=\"command-picker-description\">{3}</span>"
                "<span class=\"command-picker-app\">{4}</span></div>",
                row == active ? " active" : "",
                row,
                Platform::htmlEscape(entry.displayName),
                Platform::htmlEscape(entry.description),
                Platform::htmlEscape(entry.applicationName));
        }
    }
    host->SetProperty("display", "block");
    host->SetInnerRML(rml);
}

} // namespace Styx::UI::Rml
