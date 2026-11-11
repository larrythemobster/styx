#include "UpdatesController.hpp"
#include "ReleaseVersion.hpp"
#include "Shell.hpp"
#include "Platform/Desktop.hpp"
#include "Platform/Http.hpp"
#include "Platform/Native.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx::UI::Rml {
namespace {
constexpr const char *releases = "https://github.com/larrythemobster/styx/releases";
bool trustedRelease(const std::string &url) {
    const std::string prefix = std::string(releases) + "/tag/";
    if (!url.starts_with(prefix) || url.size() == prefix.size()) return false;
    for (size_t i = prefix.size(); i < url.size(); ++i) {
        char c = url[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '+')) return false;
    }
    return true;
}
}
UpdatesController::UpdatesController(Shell &shell, std::string endpoint)
    : Controller(shell, shell.document()), endpoint(std::move(endpoint)), releaseUrl(releases)
{
    listen("click", [this](::Rml::Event &event) {
        if (!visible) return;
        auto *target = event.GetTargetElement();
        if (target && target->GetId() == "updates-backdrop") { hide(); return; }
        auto *action = closest(target, "data-update-action");
        if (!action) return;
        auto name = attribute(action, "data-update-action");
        if (name == "close") hide();
        else if (name == "retry" && !loading) check();
        else if (name == "open") Platform::Desktop::openUrl(releaseUrl);
    });
    listen("keydown", [this](::Rml::Event &event) {
        if (!visible) return;
        int key = event.GetParameter<int>("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) { event.StopPropagation(); hide(); return; }
        if (key == ::Rml::Input::KI_TAB) {
            auto *panel = regionRoot->GetElementById("updates-panel");
            auto *context = regionRoot->GetContext();
            if (!panel || !context) return;
            ::Rml::ElementList controls;
            panel->QuerySelectorAll(controls, "button");
            std::erase_if(controls, [](auto *element) { return !element->IsVisible() || element->HasAttribute("disabled"); });
            if (controls.empty()) return;
            auto position = std::find(controls.begin(), controls.end(), context->GetFocusElement());
            int index = position == controls.end() ? -1 : static_cast<int>(position - controls.begin());
            bool backwards = event.GetParameter<bool>("shift_key", false);
            if (index < 0) index = backwards ? 0 : -1;
            index = (index + (backwards ? -1 : 1) + static_cast<int>(controls.size())) % static_cast<int>(controls.size());
            controls[index]->Focus();
            event.StopPropagation();
            return;
        }
        auto *target = event.GetTargetElement();
        if (target && target->HasAttribute("data-update-action") && (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER || key == ::Rml::Input::KI_SPACE)) {
            event.StopPropagation(); target->DispatchEvent("click", {});
        }
    });
    check();
}
UpdatesController::~UpdatesController() { if (requestId) Platform::HttpClient::instance().cancel(requestId); }
void UpdatesController::toggle() {
    if (visible) { hide(); return; }
    if (regionRoot && regionRoot->GetContext()) {
        if (auto *focused = regionRoot->GetContext()->GetFocusElement()) previousFocus = focused->GetObserverPtr();
        else previousFocus.reset();
    }
    visible = true; render();
    if (regionRoot) if (auto *close = regionRoot->GetElementById("updates-close")) close->Focus();
}
void UpdatesController::hide() {
    if (!visible) return;
    visible = false; render();
    if (auto *focus = previousFocus.get()) focus->Focus();
    previousFocus.reset();
}
void UpdatesController::check() {
    if (requestId) Platform::HttpClient::instance().cancel(requestId);
    loading = true; available = false; status = "Checking for releases..."; releaseUrl = releases; render(); changed.publish();
    Platform::HttpRequest request; request.url = endpoint;
    request.headers.emplace_back("Accept", "application/vnd.github+json");
    request.headers.emplace_back("User-Agent", "Styx/" + std::string(currentVersion));
    Platform::Guard<UpdatesController> self(this);
    requestId = Platform::HttpClient::instance().request(std::move(request), [self](const Platform::HttpResponse &response) {
        if (!self) return;
        self->requestId = 0; self->loading = false;
        if (response.statusCode == 404 && response.error.empty()) self->status = "No public release was found.";
        else if (response.statusCode == 403 || response.statusCode == 429) self->status = "Release check was denied or rate limited. Try again later.";
        else if (!response.ok()) self->status = "Could not check releases. Check your connection and try again.";
        else {
            auto data = nlohmann::json::parse(response.body, nullptr, false);
            if (!data.is_object() || !data.contains("tag_name") || !data["tag_name"].is_string()) self->status = "The release response was invalid.";
            else {
                auto tag = data["tag_name"].get<std::string>();
                auto latest = parseReleaseVersion(tag); auto current = parseReleaseVersion(currentVersion);
                if (!latest || !current) self->status = "The latest release has an unrecognized version.";
                else {
                    self->available = compareReleaseVersions(*latest, *current) > 0;
                    self->status = "Latest release: " + tag + (self->available ? " (update available)" : " (no newer release)");
                    if (data.contains("html_url") && data["html_url"].is_string()) {
                        auto url = data["html_url"].get<std::string>();
                        if (trustedRelease(url)) self->releaseUrl = std::move(url);
                    }
                }
            }
        }
        self->render(); self->changed.publish();
    });
}
void UpdatesController::render() {
    if (!regionRoot) return;
    if (auto *backdrop = regionRoot->GetElementById("updates-backdrop")) backdrop->SetProperty("display", visible ? "block" : "none");
    if (auto *current = regionRoot->GetElementById("updates-current")) current->SetInnerRML("Current version: " + std::string(currentVersion));
    if (auto *message = regionRoot->GetElementById("updates-status")) message->SetInnerRML(Platform::htmlEscape(status));
    if (auto *retry = regionRoot->GetElementById("updates-retry")) {
        if (loading) {
            if (regionRoot->GetContext() && regionRoot->GetContext()->GetFocusElement() == retry)
                if (auto *close = regionRoot->GetElementById("updates-close")) close->Focus();
            retry->SetAttribute("disabled", "");
        } else retry->RemoveAttribute("disabled");
        retry->SetPseudoClass("disabled", loading);
        retry->SetInnerRML(loading ? "Checking..." : "Check again");
    }
    requestRender();
}
}
