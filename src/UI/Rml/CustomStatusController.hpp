#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Proto/UserSettings.hpp"

#include <memory>
#include <string>

namespace Rml {
class ElementDocument;
class ElementFormControlInput;
class ElementFormControlSelect;
} // namespace Rml

namespace Styx {
namespace UI {
namespace Rml {

class ExpressionPickerController;

class CustomStatusController final : public Controller
{
public:
    explicit CustomStatusController(Shell &shell);
    ~CustomStatusController() override;

    void open();
    void close();

private:
    void ensureDocument();
    void setupEventListeners();
    void render();
    void save(bool clear);
    void chooseEmoji();

    [[nodiscard]] ::Rml::ElementFormControlInput *textInput() const;
    [[nodiscard]] ::Rml::ElementFormControlSelect *expirySelect() const;
    [[nodiscard]] ::Rml::Element *emojiButton() const;
    [[nodiscard]] ::Rml::Element *removeEmojiButton() const;
    [[nodiscard]] ::Rml::Element *clearButton() const;
    [[nodiscard]] ::Rml::Element *saveButton() const;
    [[nodiscard]] ::Rml::Element *errorElement() const;

    ::Rml::ElementDocument *customStatusDocument = nullptr;
    std::unique_ptr<ExpressionPickerController> picker;

    bool visible = false;
    bool saving = false;
    int generation = 0;
    Core::Snowflake editingAccountId;
    Proto::CustomStatus draft;
    std::string error;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
