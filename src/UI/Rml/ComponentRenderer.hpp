#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Native.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

namespace Styx::Discord { struct Message; }

namespace Styx::UI::Rml {

// All callbacks are required; only revealedSpoilers may be null.
struct ComponentRenderContext {
    std::function<std::string(const std::string &)> markdown;
    std::function<std::string(const std::string &, Platform::Size)> imageSource;
    std::function<std::string(Core::Snowflake, const std::string &)> defaultLabel;
    std::function<std::string(int64_t)> fileSize;
    const std::unordered_set<std::string> *revealedSpoilers = nullptr;
};

// Uses the visible (possibly forwarded) message; markdown returns trusted RML.
[[nodiscard]] std::string renderMessageComponents(const Discord::Message &message,
                                                const ComponentRenderContext &context);

} // namespace Styx::UI::Rml
