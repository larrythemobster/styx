#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"

#include "Platform/Timer.hpp"
#include <string>

namespace Styx {
namespace UI {
namespace Rml {

class RailController final : public Controller
{
public:
    RailController(Shell &shell, ::Rml::Element *root);

private:
    void rebuild();

    void renderRail();
    std::string renderFolderGrid(Core::Snowflake accountId, Core::Snowflake folderId);
    static std::string acronym(const std::string &name);
    void scheduleRebuild();
    Platform::Timer rebuildTimer;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
