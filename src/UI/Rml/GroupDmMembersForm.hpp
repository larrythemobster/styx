#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Signal.hpp"

#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace Styx::UI::Rml {

struct GroupDmMemberPerson
{
    Core::Snowflake userId;
    std::string displayName;
    std::string username;
    std::string avatarSrc;
    bool isSelf = false;
    bool isOwner = false;
};

struct GroupDmMembersSnapshot
{
    std::vector<GroupDmMemberPerson> members;
    std::vector<GroupDmMemberPerson> friends;
    bool canRemove = false;
};

class GroupDmMembersForm final : public ::Rml::EventListener
{
public:
    explicit GroupDmMembersForm(std::function<void()> repaint);
    ~GroupDmMembersForm() override;
    GroupDmMembersForm(const GroupDmMembersForm &) = delete;
    GroupDmMembersForm &operator=(const GroupDmMembersForm &) = delete;

    void setDocument(::Rml::ElementDocument *document);
    void reset(GroupDmMembersSnapshot snapshot);
    void update(GroupDmMembersSnapshot snapshot);
    void setPending(bool pending);
    void setError(std::string_view error);
    void focusSearch();
    void ProcessEvent(::Rml::Event &event) override;

    Platform::Signal<Core::Snowflake> addRequested;
    Platform::Signal<Core::Snowflake> removeRequested;
    Platform::Signal<> closeRequested;

private:
    void rebuildMemberRows();
    void rebuildFriendRows();
    void updateHeaderAndHint();
    void updateAllDisabledStates();
    void populateControls();

    std::function<void()> m_repaint;
    ::Rml::ElementDocument *doc = nullptr;
    GroupDmMembersSnapshot m_snapshot;
    std::string m_query;
    std::string m_error;
    bool m_pending = false;
};

} // namespace Styx::UI::Rml
