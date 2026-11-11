#pragma once

#include "Core/Result.hpp"
#include "Discord/GroupDm.hpp"
#include "Platform/Signal.hpp"

#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <string>
#include <string_view>

namespace Rml {
class ElementDocument;
}

namespace Styx::UI::Rml {

struct GroupDmDetailsSnapshot
{
    std::string name;
    std::string iconHash;
    std::string iconSrc;
};

class GroupDmDetailsForm final : public ::Rml::EventListener
{
public:
    explicit GroupDmDetailsForm(std::function<void()> repaint);
    ~GroupDmDetailsForm() override;
    GroupDmDetailsForm(const GroupDmDetailsForm &) = delete;
    GroupDmDetailsForm &operator=(const GroupDmDetailsForm &) = delete;

    void setDocument(::Rml::ElementDocument *document);
    void reset(GroupDmDetailsSnapshot snapshot);
    void updateBaseline(GroupDmDetailsSnapshot snapshot);
    void setIcon(std::string dataUri, std::string previewSource, int previewWidth, int previewHeight);
    void removeIcon();
    void setPending(bool pending);
    void setError(std::string_view error);
    void focusName();
    [[nodiscard]] bool isDirty() const;
    [[nodiscard]] Core::Result<Discord::GroupDmEdit> edit() const;

    Platform::Signal<> pickIconRequested;
    Platform::Signal<> removeIconRequested;
    Platform::Signal<> resetRequested;
    Platform::Signal<> submitRequested;
    Platform::Signal<> closeRequested;

    void ProcessEvent(::Rml::Event &event) override;

private:
    struct IconDraft
    {
        std::string dataUri;
        std::string previewSource;
        int previewWidth = 0;
        int previewHeight = 0;
        bool removed = false;
    };

    void renderIcon();
    void updateDisabledStates();
    void populateControls();
    [[nodiscard]] bool isNameDirty() const;
    [[nodiscard]] bool isIconDirty() const;
    [[nodiscard]] bool isNameValid() const;
    [[nodiscard]] bool hasActiveIcon() const;
    [[nodiscard]] std::string_view nameError() const;

    std::function<void()> m_repaint;
    ::Rml::ElementDocument *doc = nullptr;
    GroupDmDetailsSnapshot m_baseline;
    std::string m_nameDraft;
    IconDraft m_iconDraft;
    std::string m_error;
    bool m_pending = false;
};

} // namespace Styx::UI::Rml
