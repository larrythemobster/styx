#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/InviteLink.hpp"

#include <optional>
#include <string>

namespace Rml {
class ElementDocument;
class ElementFormControlInput;
}

namespace Styx {
namespace UI {
namespace Rml {

// The "Join a Server" dialog: paste an invite, see where it leads, join.
class JoinServerController final : public Controller
{
public:
    explicit JoinServerController(Shell &shell);
    ~JoinServerController() override;

    // With an empty code the dialog asks for an invite link; otherwise it looks the code up
    // straight away. location is sent as Discord's context properties when joining.
    void open(const std::string &code, const std::string &location);
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    enum class State {
        Entering,
        Looking,
        Preview,
        Joining
    };

    void ensureDocument();
    void setupEventListeners();
    void render();
    void lookUp(const std::string &code);
    // Back to the input with message under it.
    void showError(std::string message);
    // Join, or open the server when the account is already in it.
    void confirm();
    [[nodiscard]] bool alreadyMember() const;
    [[nodiscard]] ::Rml::ElementFormControlInput *input() const;

    ::Rml::ElementDocument *joinDocument = nullptr;
    bool visible = false;
    State state = State::Entering;
    std::string location;
    std::optional<Discord::InvitePreview> preview;
    std::string error;
    // Drops replies to a lookup or join from an earlier opening of the dialog.
    int generation = 0;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
