#pragma once

#include "Controller.hpp"
#include "Core/QuickSwitcher.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
class ElementFormControlInput;
}

namespace Styx {
namespace UI {
namespace Rml {

// The Ctrl+K quick switcher: jump to a server, channel, DM or friend of the current account.
class SwitcherController final : public Controller
{
public:
    // Listens for Ctrl+K on the main document, before focused elements see it.
    explicit SwitcherController(Shell &shell);
    ~SwitcherController() override;

    void open();
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    struct Row
    {
        Core::SwitcherItem item;
        // Grey text after the name: the server a channel is in, "Direct Message" or "Friend".
        std::string detail;
        // An img src: a server icon or avatar URL through Shell::imageSource, or an icon: name.
        std::string image;
    };

    void ensureDocument();
    void setupEventListeners();
    // Rebuilds rows from the current account's channel tree and friends.
    void collectRows();
    // Re-ranks for the box text, selects the first result and renders.
    void updateResults();
    void render();
    void moveSelection(int delta);
    void activate(std::size_t rankedIndex);
    [[nodiscard]] ::Rml::ElementFormControlInput *input() const;
    [[nodiscard]] std::string inputText() const;

    ::Rml::ElementDocument *switcherDocument = nullptr;
    bool visible = false;
    std::vector<Row> rows;
    // Indices into rows, best first.
    std::vector<std::size_t> ranked;
    int selected = 0;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
