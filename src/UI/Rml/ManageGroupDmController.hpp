#pragma once

#include "Controller.hpp"
#include "Shell.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace Styx::Discord {
struct Channel;
}

namespace Styx::UI::Rml {

class GroupDmMembersForm;
struct GroupDmMembersSnapshot;

class ManageGroupDmController final : public Controller
{
public:
    explicit ManageGroupDmController(Shell &shell);
    ~ManageGroupDmController() override;

    void open(Core::Snowflake channelId);
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }
    static bool canOpen(Core::ClientInstance *instance, Core::Snowflake channelId);

private:
    void ensureDocument();
    enum class Request { Add, Remove };
    void submitRequest(Core::Snowflake userId, Request request);
    void refresh();
    GroupDmMembersSnapshot buildSnapshot(Core::ClientInstance &instance, const Discord::Channel &channel);
    void finishRequest(std::uint64_t requestGeneration, const Platform::Guard<Core::ClientInstance> &origin,
                       const UI::Location &location, bool success, std::string_view error);

    ::Rml::ElementDocument *document = nullptr;
    std::unique_ptr<GroupDmMembersForm> form;
    bool visible = false;
    bool pending = false;
    std::uint64_t generation = 0;
    Platform::Guard<Core::ClientInstance> originInstance;
    UI::Location originLocation;
    Core::Snowflake currentChannelId;
    std::vector<Platform::Connection> instanceConnections;
};

} // namespace Styx::UI::Rml
