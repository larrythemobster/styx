#pragma once

#include "Controller.hpp"

#include <string>

namespace Rml {
class ElementDocument;
class ElementFormControlInput;
}

namespace Styx {
namespace UI {
namespace Rml {

// "Customize Your Server": pick a name and an optional icon, create the server and open it.
class CreateServerController final : public Controller
{
public:
    explicit CreateServerController(Shell &shell);
    ~CreateServerController() override;

    void open();
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    void ensureDocument();
    void setupEventListeners();
    void render();
    void pickIcon();
    void create();
    [[nodiscard]] std::string defaultName() const;
    [[nodiscard]] ::Rml::ElementFormControlInput *nameInput() const;

    ::Rml::ElementDocument *createDocument = nullptr;
    bool visible = false;
    bool creating = false;
    std::string iconDataUri;
    std::string error;
    // Drops a reply to a create from an earlier opening of the dialog.
    int generation = 0;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
