#pragma once

#include "Controller.hpp"
#include "Shell.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace Styx::UI::Rml {

class GroupDmCreateForm;
struct GroupDmFriend;

class CreateGroupDmController final : public Controller
{
public:
    explicit CreateGroupDmController(Shell &shell);
    ~CreateGroupDmController() override;

    void open(Core::Snowflake sourceChannelId = Core::Snowflake::Invalid);
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }
    static bool canOpen(Core::ClientInstance *instance, Core::Snowflake sourceChannelId = Core::Snowflake::Invalid);

private:
    void ensureDocument();
    void submit();
    void refreshCandidates();
    std::vector<GroupDmFriend> buildCandidates(Core::ClientInstance &instance);
    static std::optional<Core::Snowflake> resolveSourceRecipient(Core::ClientInstance &instance, Core::Snowflake channelId);
    void finishRequest(std::uint64_t requestGeneration, const Platform::Guard<Core::ClientInstance> &origin,
                       const UI::Location &location, Core::Snowflake createdId, std::string_view error);

    ::Rml::ElementDocument *document = nullptr;
    std::unique_ptr<GroupDmCreateForm> form;
    bool visible = false;
    bool pending = false;
    std::uint64_t generation = 0;
    Platform::Guard<Core::ClientInstance> originInstance;
    UI::Location originLocation;
    Core::Snowflake sourceChannelId;
    Core::Snowflake sourceOtherRecipient;
    std::vector<Platform::Connection> instanceConnections;
};

} // namespace Styx::UI::Rml
