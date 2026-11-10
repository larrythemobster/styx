#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Signal.hpp"

#include <RmlUi/Core/EventListener.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace Styx::UI::Rml {

struct GroupDmFriend
{
    Core::Snowflake userId;
    std::string displayName;
    std::string username;
    std::string avatarSrc;
};

class GroupDmCreateForm final : public ::Rml::EventListener
{
public:
    explicit GroupDmCreateForm(std::function<void()> repaint);
    ~GroupDmCreateForm() override;
    GroupDmCreateForm(const GroupDmCreateForm &) = delete;
    GroupDmCreateForm &operator=(const GroupDmCreateForm &) = delete;

    void setDocument(::Rml::ElementDocument *document);
    void reset(std::vector<GroupDmFriend> friends, std::size_t maximumSelections, bool allowEmpty);
    void setCandidates(std::vector<GroupDmFriend> friends);
    [[nodiscard]] const std::vector<Core::Snowflake> &selected() const;
    void setPending(bool pending);
    void setError(std::string_view error);
    void focusSearch();

    Platform::Signal<> submitRequested;
    Platform::Signal<> cancelRequested;

    void ProcessEvent(::Rml::Event &event) override;

private:
    void rebuildFriendRows();
    void updateAllDisabledStates();
    void updateSubmitButton();
    void updateContextAndCount();
    void populateControls();
    void toggleFriend(const Core::Snowflake &userId);
    [[nodiscard]] bool canSubmit() const;

    std::function<void()> m_repaint;
    ::Rml::ElementDocument *doc = nullptr;
    std::vector<GroupDmFriend> m_friends;
    std::vector<Core::Snowflake> m_selected;
    std::string m_query;
    std::string m_error;
    std::size_t m_maximumSelections = 0;
    bool m_allowEmpty = true;
    bool m_pending = false;
};

} // namespace Styx::UI::Rml
