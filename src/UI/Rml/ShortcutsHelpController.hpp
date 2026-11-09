#pragma once

#include "Controller.hpp"

namespace Rml {
class ElementDocument;
}

namespace Styx {
namespace UI {
namespace Rml {

// The Ctrl+/ dialog listing every keyboard shortcut, read from shortcutTable().
class ShortcutsHelpController final : public Controller
{
public:
    explicit ShortcutsHelpController(Shell &shell);
    ~ShortcutsHelpController() override;

    void open();
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    void ensureDocument();
    void setupEventListeners();

    ::Rml::ElementDocument *helpDocument = nullptr;
    bool visible = false;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
