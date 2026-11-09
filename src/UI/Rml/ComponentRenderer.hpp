#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Native.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Styx::Discord { struct Message; struct MessageComponent; }

namespace Styx::UI::Rml {

// All callbacks are required; only revealedSpoilers may be null.
struct ComponentRenderContext {
    std::function<std::string(const std::string &)> markdown;
    std::function<std::string(const std::string &, Platform::Size)> imageSource;
    std::function<std::string(Core::Snowflake, const std::string &)> defaultLabel;
    std::function<std::string(int64_t)> fileSize;
    const std::unordered_set<std::string> *revealedSpoilers = nullptr;
    // Buttons with a custom_id can be clicked; forwarded messages stay read-only.
    bool interactive = false;
    // The button whose interaction is waiting on the bot.
    std::string pendingPath;
};

// Uses the visible (possibly forwarded) message; markdown returns trusted RML.
[[nodiscard]] std::string renderMessageComponents(const Discord::Message &message,
                                                const ComponentRenderContext &context);

// The component a rendered data-component-path names, in the visible message; null for
// gallery items or a path the message no longer has.
[[nodiscard]] const Discord::MessageComponent *componentAtPath(const Discord::Message &message,
                                                             std::string_view path);

} // namespace Styx::UI::Rml
