#include "CommandOptionsBar.hpp"

#include "Platform/Native.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Styx::UI::Rml {

namespace {

std::string renderOption(std::string_view value, std::string_view label, bool selected = false, bool disabled = false)
{
    return std::format("<option value=\"{0}\"{1}{2}>{3}</option>",
        Platform::htmlEscape(value),
        selected ? " selected" : "",
        disabled ? " disabled" : "",
        Platform::htmlEscape(label));
}

std::string renderSelect(std::size_t index, bool pending, const std::string &options)
{
    return std::format("<select id=\"command-option-{0}\" class=\"command-option-select\"{1}>{2}</select>",
                       index, pending ? " disabled" : "", options);
}

// Focuses a control the bar just rebuilt and, for text fields, selects start..end
// (-1 for both puts the caret at the end).
void focusControl(::Rml::Element *host, const std::string &id, int start, int end)
{
    ::Rml::Element *control = host ? host->GetElementById(id) : nullptr;
    if (!control)
        return;
    control->Focus();
    if (auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(control)) {
        if (start < 0) {
            start = static_cast<int>(input->GetValue().size());
            end = start;
        }
        input->SetSelectionRange(start, end);
    }
}

bool hasTextField(const Discord::ApplicationCommandOption &opt)
{
    return opt.choices.empty()
        && opt.type != Discord::ApplicationCommandOptionType::BOOLEAN
        && opt.type != Discord::ApplicationCommandOptionType::USER
        && opt.type != Discord::ApplicationCommandOptionType::ROLE
        && opt.type != Discord::ApplicationCommandOptionType::MENTIONABLE
        && opt.type != Discord::ApplicationCommandOptionType::CHANNEL
        && opt.type != Discord::ApplicationCommandOptionType::ATTACHMENT;
}

} // namespace

void CommandOptionsBar::setHost(::Rml::Element *newHost)
{
    if (host)
        readDrafts();
    host = newHost;
    render();
}

void CommandOptionsBar::show(const SlashCommandEntry &newCommand, std::vector<std::vector<SelectCandidate>> newCandidates)
{
    command = newCommand;
    candidates = std::move(newCandidates);
    drafts.assign(command.options.size(), {});
    optionErrors.clear();
    commandError.clear();
    pending = false;
    open = true;
    suggestionOption = 0;
    suggestions.clear();
    render();
}

void CommandOptionsBar::hide()
{
    open = false;
    pending = false;
    command = {};
    candidates.clear();
    drafts.clear();
    optionErrors.clear();
    commandError.clear();
    suggestionOption = 0;
    suggestions.clear();
    render();
}

std::vector<std::string> CommandOptionsBar::values()
{
    readDrafts();
    return drafts;
}

void CommandOptionsBar::setErrors(std::vector<std::string> newOptionErrors, std::string newCommandError)
{
    readDrafts();
    optionErrors = std::move(newOptionErrors);
    commandError = std::move(newCommandError);
    render();
}

void CommandOptionsBar::setPending(bool isPending)
{
    readDrafts();
    pending = isPending;
    render();
}

void CommandOptionsBar::setSuggestions(std::size_t optionIndex, std::vector<Discord::ApplicationCommandChoice> choices)
{
    if (!open || optionIndex >= command.options.size() || !hasTextField(command.options[optionIndex]))
        return;

    readDrafts();
    suggestionOption = optionIndex;
    suggestions = std::move(choices);
    render();
}

bool CommandOptionsBar::chooseSuggestion(std::size_t optionIndex, std::size_t choice)
{
    if (!open || optionIndex != suggestionOption || choice >= suggestions.size())
        return false;

    readDrafts();
    const auto &value = suggestions[choice].value;
    drafts[optionIndex] = value.is_string() ? value.get<std::string>() : value.dump();
    suggestions.clear();
    render();
    // Clicking the row moved focus off the field; put it back so typing carries on.
    focusControl(host, "command-option-" + std::to_string(optionIndex), -1, -1);
    return true;
}

void CommandOptionsBar::readDrafts()
{
    if (!open || !host)
        return;

    for (std::size_t i = 0; i < command.options.size(); ++i) {
        if (auto *el = host->QuerySelector("#command-option-" + std::to_string(i))) {
            if (auto *control = rmlui_dynamic_cast<::Rml::ElementFormControl *>(el))
                drafts[i] = control->GetValue();
        }
    }
}

void CommandOptionsBar::render()
{
    if (!host)
        return;
    if (!open) {
        host->SetProperty("display", "none");
        host->SetInnerRML("");
        return;
    }

    std::string rml = std::format(
        "<div class=\"command-options-title\">/{0}</div>",
        Platform::htmlEscape(command.displayName));

    for (std::size_t i = 0; i < command.options.size(); ++i) {
        const auto &opt = command.options[i];
        std::string controlHtml;

        if (hasTextField(opt)) {
            controlHtml = std::format(
                "<input type=\"text\" id=\"command-option-{0}\" class=\"command-option-input\" value=\"{1}\"{2}/>",
                i, Platform::htmlEscape(drafts[i]), pending ? " disabled" : "");
            if (i == suggestionOption && !suggestions.empty()) {
                controlHtml += "<div class=\"command-suggestions\">";
                for (std::size_t k = 0; k < suggestions.size(); ++k) {
                    controlHtml += std::format(
                        "<div class=\"command-suggestion\" data-action=\"command-suggestion\" data-option=\"{0}\" data-choice=\"{1}\">{2}</div>",
                        i, k, Platform::htmlEscape(suggestions[k].name));
                }
                controlHtml += "</div>";
            }
        } else if (!opt.choices.empty()) {
            std::string body = renderOption("", "Choose…", drafts[i].empty());
            for (const auto &choice : opt.choices)
                body += renderOption(choice.name, choice.name, drafts[i] == choice.name);
            controlHtml = renderSelect(i, pending, body);
        } else if (opt.type == Discord::ApplicationCommandOptionType::BOOLEAN) {
            const std::string body = renderOption("", "Choose…", drafts[i].empty())
                + renderOption("true", "True", drafts[i] == "true")
                + renderOption("false", "False", drafts[i] == "false");
            controlHtml = renderSelect(i, pending, body);
        } else if (opt.type == Discord::ApplicationCommandOptionType::USER
            || opt.type == Discord::ApplicationCommandOptionType::ROLE
            || opt.type == Discord::ApplicationCommandOptionType::MENTIONABLE
            || opt.type == Discord::ApplicationCommandOptionType::CHANNEL) {
            std::string body;
            if (i < candidates.size() && !candidates[i].empty()) {
                body += renderOption("", "Choose…", drafts[i].empty());
                for (const auto &cand : candidates[i]) {
                    const std::string label = (cand.kind == "channel" ? "#" : "@") + cand.label;
                    body += renderOption(cand.value, label, drafts[i] == cand.value);
                }
            } else {
                body = renderOption("", "Nothing to pick here", false, true);
            }
            controlHtml = renderSelect(i, pending, body);
        } else if (opt.type == Discord::ApplicationCommandOptionType::ATTACHMENT) {
            controlHtml = "<div class=\"command-option-unsupported\">Files aren't supported yet</div>";
        }

        const std::string labelText = Platform::htmlEscape(opt.name) + (opt.required ? " *" : "");
        rml += std::format(
            "<div class=\"command-option\">"
            "<div class=\"command-option-label\" title=\"{0}\">{1}</div>"
            "{2}",
            Platform::htmlEscape(opt.description),
            labelText,
            controlHtml);

        if (i < optionErrors.size() && !optionErrors[i].empty()) {
            rml += std::format("<div class=\"command-option-error\">{0}</div>",
                Platform::htmlEscape(optionErrors[i]));
        }
        rml += "</div>";
    }

    if (!commandError.empty())
        rml += std::format("<div class=\"command-options-error\">{0}</div>", Platform::htmlEscape(commandError));
    if (pending)
        rml += "<div class=\"command-options-status\">Sending…</div>";
    else
        rml += "<div class=\"command-options-hint\">Enter to send · Escape to cancel</div>";

    // Re-rendering replaces every control, so put focus back where the person was typing;
    // suggestions arrive mid-word.
    ::Rml::Context *context = host->GetContext();
    ::Rml::Element *focused = context ? context->GetFocusElement() : nullptr;
    std::string focusedId;
    int selectionStart = -1;
    int selectionEnd = -1;
    if (focused && !focused->GetId().empty() && host->GetElementById(focused->GetId()) == focused) {
        focusedId = focused->GetId();
        if (auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(focused))
            input->GetSelection(&selectionStart, &selectionEnd, nullptr);
    }

    host->SetProperty("display", "block");
    host->SetInnerRML(rml);

    if (!focusedId.empty())
        focusControl(host, focusedId, selectionStart, selectionEnd);
}

} // namespace Styx::UI::Rml
