#pragma once
#include "Controller.hpp"
#include "Platform/Signal.hpp"
#include <RmlUi/Core/ObserverPtr.h>
#include <cstdint>
#include <string>

namespace Styx::UI::Rml {
class UpdatesController final : public Controller {
public:
    explicit UpdatesController(Shell &shell, std::string endpoint = "https://api.github.com/repos/larrythemobster/styx/releases/latest");
    ~UpdatesController() override;
    void toggle();
    void hide();
    bool isOpen() const { return visible; }
    bool updateAvailable() const { return available; }
    Platform::Signal<> changed;
private:
    void check();
    void render();
    std::string endpoint, status, releaseUrl;
    uint64_t requestId = 0;
    bool visible = false, available = false, loading = false;
    ::Rml::ObserverPtr<::Rml::Element> previousFocus;
};
}
