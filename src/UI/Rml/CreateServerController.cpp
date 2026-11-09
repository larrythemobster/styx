#include "CreateServerController.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"
#include "RmlRuntime.hpp"
#include "ImageUpload.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Result.hpp"
#include "Core/UserManager.hpp"
#include "Discord/Client.hpp"
#include "Platform/Desktop.hpp"
#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include "Platform/Utf8.hpp"

#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx {
namespace UI {
namespace Rml {

CreateServerController::CreateServerController(Shell &shell)
    : Controller(shell, nullptr)
{
}

CreateServerController::~CreateServerController()
{
    Runtime::instance().clearFrame("create-server-icon");
    if (createDocument)
        createDocument->Close();
}

void CreateServerController::open()
{
    ensureDocument();
    if (!createDocument)
        return;

    ++generation;
    creating = false;
    iconDataUri.clear();
    error.clear();
    Runtime::instance().clearFrame("create-server-icon");

    if (auto *inputEl = nameInput())
        inputEl->SetValue(defaultName());

    visible = true;
    createDocument->Show(::Rml::ModalFlag::None);

    if (auto *inputEl = nameInput())
        inputEl->Focus();

    render();
}

void CreateServerController::close()
{
    if (!visible)
        return;

    visible = false;
    ++generation;
    if (createDocument)
        createDocument->Hide();
    requestRender();
}

void CreateServerController::ensureDocument()
{
    if (createDocument)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    createDocument = view->context()->LoadDocument("ui/create_server.rml");
    if (!createDocument)
        return;

    regionRoot = createDocument;
    setupEventListeners();
}

void CreateServerController::setupEventListeners()
{
    listen("click", [this](::Rml::Event &event) {
        auto *actionEl = closest(event.GetTargetElement(), "data-action");
        if (!actionEl)
            return;

        const std::string action = attribute(actionEl, "data-action");
        if (action == "close") {
            close();
        } else if (action == "pick-icon") {
            pickIcon();
        } else if (action == "create") {
            create();
        } else if (action == "join") {
            close();
            shell.openJoinServer({}, "Join Guild");
        }
    });

    listen("keydown", [this](::Rml::Event &event) {
        if (event.GetParameter<int>("key_identifier", 0) != ::Rml::Input::KI_ESCAPE)
            return;
        event.StopPropagation();
        close();
    });

    if (auto *inputEl = nameInput()) {
        listen("keydown", [this](::Rml::Event &event) {
            const int key = event.GetParameter<int>("key_identifier", 0);
            if (key != ::Rml::Input::KI_RETURN && key != ::Rml::Input::KI_NUMPADENTER)
                return;
            event.StopPropagation();
            create();
        }, inputEl);
    }
}

void CreateServerController::render()
{
    if (!createDocument)
        return;

    if (auto *iconEl = createDocument->GetElementById("create-server-icon")) {
        const bool picked = !iconDataUri.empty();
        iconEl->SetClass("picked", picked);
        if (picked)
            iconEl->SetInnerRML("<img class=\"create-server-picked\" src=\"frame:create-server-icon\"/>");
        else
            iconEl->SetInnerRML("<img class=\"create-server-camera\" src=\"icon:camera\"/><div class=\"create-server-upload\">UPLOAD</div>");
    }

    if (auto *errorEl = createDocument->GetElementById("create-server-error")) {
        errorEl->SetInnerRML(Platform::htmlEscape(error));
        errorEl->SetProperty("display", error.empty() ? "none" : "block");
    }

    if (auto *confirmBtn = createDocument->GetElementById("create-server-confirm")) {
        confirmBtn->SetInnerRML(creating ? "Creating…" : "Create");
        if (creating)
            confirmBtn->SetAttribute("disabled", "");
        else
            confirmBtn->RemoveAttribute("disabled");
    }

    requestRender();
}

void CreateServerController::pickIcon()
{
    const int gen = generation;
    Platform::Desktop::openFiles("Choose Server Icon", {{"Images", "*.png;*.jpg;*.jpeg;*.gif;*.webp;*.avif;*.bmp"}}, [this, guard = lifetime(), gen](std::vector<std::string> files) {
        if (guard.expired() || gen != generation || files.empty())
            return;

        auto prepared = ImageUpload::prepareCropped(files.front(), 1024, 1024);
        if (prepared.dataUri.empty()) {
            error = std::move(prepared.error);
        } else {
            iconDataUri = std::move(prepared.dataUri);
            error.clear();
            if (auto buffer = Platform::Image::decodeFile(files.front()))
                Runtime::instance().setFrame("create-server-icon", *buffer);
        }
        render();
    }, false);
}

void CreateServerController::create()
{
    if (creating)
        return;

    auto *inputEl = nameInput();
    const std::string name = inputEl ? std::string(Platform::trim(inputEl->GetValue())) : std::string();
    if (name.empty()) {
        error = "Your server needs a name.";
        render();
        return;
    }

    auto *instance = shell.currentInstance();
    if (!instance || !instance->discord())
        return;

    creating = true;
    error.clear();
    render();

    const auto accountId = instance->accountId();
    const int gen = generation;
    instance->discord()->createGuild(name, iconDataUri, [this, guard = lifetime(), gen, accountId](const Core::Result<Core::Snowflake> &result) {
        if (guard.expired() || gen != generation)
            return;

        if (result.success() && result.value) {
            shell.openGuildWhenReady(accountId, *result.value);
            close();
        } else {
            creating = false;
            error = result.error;
            render();
        }
    });
}

std::string CreateServerController::defaultName() const
{
    auto *instance = shell.currentInstance();
    if (instance && instance->users()) {
        const std::string name = instance->users()->getDisplayName(instance->accountId());
        if (!name.empty())
            return name + "'s server";
    }
    return "My server";
}

::Rml::ElementFormControlInput *CreateServerController::nameInput() const
{
    if (!createDocument)
        return nullptr;
    return rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(createDocument->GetElementById("create-server-name"));
}

} // namespace Rml
} // namespace UI
} // namespace Styx
