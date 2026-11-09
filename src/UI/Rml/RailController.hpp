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
    // Up to four initials, as Discord shows for a server without an icon.
    static std::string acronym(const std::string &name);

private:
    void rebuild();

    void renderRail();
    std::string renderFolderGrid(Core::Snowflake accountId, Core::Snowflake folderId);
    void scheduleRebuild();
    Platform::Timer rebuildTimer;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
