#include "JoinServerController.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"
#include "RailController.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Result.hpp"
#include "Discord/ApiError.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"
#include "Discord/InviteLink.hpp"
#include "Platform/Native.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"

#include <format>
#include <string>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx {
namespace UI {
namespace Rml {

JoinServerController::JoinServerController(Shell &shell)
    : Controller(shell, nullptr)
{
}

JoinServerController::~JoinServerController()
{
    if (joinDocument)
        joinDocument->Close();
}

void JoinServerController::open(const std::string &code, const std::string &location)
{
    ensureDocument();
    if (!joinDocument)
        return;

    this->location = location;
    ++generation;
    preview.reset();
    error.clear();

    visible = true;
    joinDocument->Show(::Rml::ModalFlag::None);

    auto *inputEl = input();
    if (inputEl) {
        inputEl->Focus();
        inputEl->SetValue(code);
    }

    state = State::Entering;
    if (!code.empty()) {
        lookUp(code);
        return;
    }
    render();
}

void JoinServerController::close()
{
    if (!visible)
        return;

    visible = false;
    ++generation;
    if (joinDocument)
        joinDocument->Hide();
    requestRender();
}

void JoinServerController::ensureDocument()
{
    if (joinDocument)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    joinDocument = view->context()->LoadDocument("ui/join_server.rml");
    if (!joinDocument)
        return;

    regionRoot = joinDocument;
    setupEventListeners();
}

void JoinServerController::setupEventListeners()
{
    listen("click", [this](::Rml::Event &event) {
        auto *actionEl = closest(event.GetTargetElement(), "data-action");
        if (!actionEl)
            return;

        const std::string action = attribute(actionEl, "data-action");
        if (action == "close")
            close();
        else if (action == "confirm")
            confirm();
    });

    listen("keydown", [this](::Rml::Event &event) {
        if (event.GetParameter<int>("key_identifier", 0) != ::Rml::Input::KI_ESCAPE)
            return;
        event.StopPropagation();
        close();
    });

    if (auto *inputEl = input()) {
        // Editing the invite after a lookup makes the button look the new text up.
        listen("change", [this](::Rml::Event &) {
            if (state != State::Preview)
                return;
            state = State::Entering;
            preview.reset();
            render();
        }, inputEl);

        listen("keydown", [this](::Rml::Event &event) {
            const int key = event.GetParameter<int>("key_identifier", 0);
            if (key != ::Rml::Input::KI_RETURN && key != ::Rml::Input::KI_NUMPADENTER)
                return;
            event.StopPropagation();
            confirm();
        }, inputEl);
    }
}

void JoinServerController::render()
{
    if (!joinDocument)
        return;

    if (auto *previewEl = joinDocument->GetElementById("join-server-preview")) {
        if (preview) {
            std::string html;
            if (!preview->guildIcon.empty()) {
                const std::string iconUrl = shell.imageSource(Discord::Cdn::guildIcon(preview->guildId, preview->guildIcon, 64), Platform::Size{48, 48});
                html += std::format("<img class=\"join-server-icon\" src=\"{}\"/>", Platform::htmlEscape(iconUrl));
            } else {
                html += std::format("<div class=\"join-server-icon join-server-acronym\">{}</div>", Platform::htmlEscape(RailController::acronym(preview->guildName)));
            }

            html += "<div class=\"join-server-info\">";
            html += std::format("<div class=\"join-server-name\">{}</div>", Platform::htmlEscape(preview->guildName));
            html += std::format("<div class=\"join-server-counts\"><span class=\"join-server-dot online\"></span>{} Online<span class=\"join-server-dot\"></span>{} Members</div>",
                                preview->onlineCount, preview->memberCount);
            if (!preview->inviterName.empty() || !preview->channelName.empty()) {
                std::string meta = preview->inviterName.empty() ? std::string() : "Invited by " + preview->inviterName;
                if (!preview->channelName.empty())
                    meta += (meta.empty() ? "#" : " to #") + preview->channelName;
                html += std::format("<div class=\"join-server-meta\">{}</div>", Platform::htmlEscape(meta));
            }
            html += "</div>";

            previewEl->SetInnerRML(html);
            previewEl->SetProperty("display", "flex");
        } else {
            previewEl->SetProperty("display", "none");
            previewEl->SetInnerRML("");
        }
    }

    if (auto *errorEl = joinDocument->GetElementById("join-server-error")) {
        errorEl->SetInnerRML(Platform::htmlEscape(error));
        errorEl->SetProperty("display", error.empty() ? "none" : "block");
    }

    if (auto *confirmBtn = joinDocument->GetElementById("join-server-confirm")) {
        const bool busy = state == State::Looking || state == State::Joining;
        const char *label = state == State::Looking ? "Looking up…"
                            : state == State::Joining ? "Joining…"
                            : alreadyMember()         ? "Go to Server"
                                                      : "Join Server";
        confirmBtn->SetInnerRML(label);
        if (busy)
            confirmBtn->SetAttribute("disabled", "");
        else
            confirmBtn->RemoveAttribute("disabled");
    }

    requestRender();
}

void JoinServerController::lookUp(const std::string &code)
{
    const auto parsed = Discord::parseInviteCode(code);
    if (!parsed) {
        showError("That doesn't look like an invite link.");
        return;
    }

    auto *instance = shell.currentInstance();
    if (!instance || !instance->discord()) {
        showError("Not connected.");
        return;
    }

    state = State::Looking;
    preview.reset();
    error.clear();
    render();

    const int gen = ++generation;
    instance->discord()->resolveInvite(*parsed, [this, guard = lifetime(), gen](const Core::Result<Discord::InvitePreview> &result) {
        if (guard.expired() || gen != generation)
            return;
        if (!result.success()) {
            showError(result.code == Discord::ApiError::UnknownInvite ? std::string("This invite is invalid or has expired.") : result.error);
            return;
        }
        preview = result.value;
        state = State::Preview;
        render();
    });
}

void JoinServerController::showError(std::string message)
{
    state = State::Entering;
    preview.reset();
    error = std::move(message);
    render();
}

void JoinServerController::confirm()
{
    if (state == State::Entering) {
        auto *inputEl = input();
        lookUp(inputEl ? inputEl->GetValue() : std::string());
        return;
    }
    if (state != State::Preview)
        return;

    auto *instance = shell.currentInstance();
    if (!instance || !instance->discord())
        return;

    if (alreadyMember()) {
        shell.openGuild(instance->accountId(), preview->guildId);
        close();
        return;
    }

    state = State::Joining;
    error.clear();
    render();

    const int gen = ++generation;
    const Core::Snowflake previewGuildId = preview->guildId;
    instance->discord()->acceptInvite(preview->code, location, [this, guard = lifetime(), gen, accountId = instance->accountId(), previewGuildId](const Core::Result<Core::Snowflake> &result) {
        if (guard.expired() || gen != generation)
            return;
        if (!result.success()) {
            state = State::Preview;
            error = result.error;
            render();
            return;
        }
        const Core::Snowflake joined = result.value.value_or(Core::Snowflake::Invalid);
        shell.openGuildWhenReady(accountId, joined.isValid() ? joined : previewGuildId);
        close();
    });
}

bool JoinServerController::alreadyMember() const
{
    auto *instance = shell.currentInstance();
    auto *tree = shell.channelTree();
    return preview && instance && tree && tree->serverNode(instance->accountId(), preview->guildId);
}

::Rml::ElementFormControlInput *JoinServerController::input() const
{
    if (!joinDocument)
        return nullptr;
    return rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(joinDocument->GetElementById("join-server-input"));
}

} // namespace Rml
} // namespace UI
} // namespace Styx
