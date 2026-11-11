#pragma once

#include "Controller.hpp"
#include "Shell.hpp"
#include "Core/Result.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace Styx::Discord {
struct Channel;
}

namespace Styx::UI::Rml {

class GroupDmDetailsForm;
struct GroupDmDetailsSnapshot;

class EditGroupDmController final : public Controller
{
public:
    explicit EditGroupDmController(Shell &shell);
    ~EditGroupDmController() override;

    void open(Core::Snowflake channelId);
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    void ensureDocument();
    std::optional<Discord::Channel> currentGroup();
    void refreshBaseline();
    void chooseIcon();
    void handleRemoveIcon();
    void handleReset();
    void save();
    void finishSave(std::uint64_t requestGen, const Platform::Guard<Core::ClientInstance> &origin,
                    const UI::Location &location, Core::Snowflake channelId,
                    const Core::Result<Discord::Channel> &result);
    GroupDmDetailsSnapshot buildSnapshot(const Discord::Channel &channel) const;

    ::Rml::ElementDocument *document = nullptr;
    std::unique_ptr<GroupDmDetailsForm> form;
    bool visible = false;
    bool pending = false;
    std::uint64_t requestGeneration = 0;
    std::uint64_t pickerGeneration = 0;
    std::string uniqueFrameKey;
    Platform::Guard<Core::ClientInstance> originInstance;
    UI::Location originLocation;
    Core::Snowflake currentChannelId;
    std::vector<Platform::Connection> instanceConnections;
};

} // namespace Styx::UI::Rml
