#pragma once

#include "Discord/PollCreate.hpp"
#include "Platform/Native.hpp"
#include "Platform/Signal.hpp"

#include <RmlUi/Core/EventListener.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Rml {
class ElementDocument;
}

namespace Styx::UI::Rml {

class PollCreateForm final : public ::Rml::EventListener
{
public:
    using ImageSource = std::function<std::string(const std::string &, Platform::Size)>;

    PollCreateForm(ImageSource imageSource, std::function<void()> repaint);
    ~PollCreateForm() override;
    PollCreateForm(const PollCreateForm &) = delete;
    PollCreateForm &operator=(const PollCreateForm &) = delete;

    void setDocument(::Rml::ElementDocument *document);
    void reset();
    [[nodiscard]] const Discord::PollCreateRequest &draft() const;
    void setPending(bool pending);
    void setError(std::string_view error);
    void setEmoji(std::size_t index, std::optional<Discord::Emoji> emoji);
    void focusQuestion();
    void focusEmoji(std::size_t index);
    [[nodiscard]] Platform::Rect emojiAnchor(std::size_t index) const;

    Platform::Signal<> changed;
    Platform::Signal<> submitRequested;
    Platform::Signal<> cancelRequested;
    Platform::Signal<std::size_t> emojiRequested;

    void ProcessEvent(::Rml::Event &event) override;

private:
    void rebuildAnswerRows();
    void updateAllDisabledStates();
    void updateSubmitButton();
    void populateControls();
    void draftChanged();
    [[nodiscard]] bool canSubmit() const;

    ImageSource m_imageSource;
    std::function<void()> m_repaint;
    ::Rml::ElementDocument *doc = nullptr;
    Discord::PollCreateRequest m_draft;
    std::string m_error;
    bool m_pending = false;
};

} // namespace Styx::UI::Rml
