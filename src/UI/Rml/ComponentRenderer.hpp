#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Native.hpp"
#include "UI/Chat/SelectDraft.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Styx::Discord { struct Message; struct MessageComponent; }

namespace Styx::UI::Rml {

// All callbacks are required; only revealedSpoilers and openSelectValues may be null.
struct ComponentRenderContext {
    std::function<std::string(const std::string &)> markdown;
    std::function<std::string(const std::string &, Platform::Size)> imageSource;
    std::function<std::string(Core::Snowflake, const std::string &)> defaultLabel;
    std::function<std::string(int64_t)> fileSize;
    const std::unordered_set<std::string> *revealedSpoilers = nullptr;
    // Buttons and string selects with a custom_id can be used; forwarded messages stay
    // read-only.
    bool interactive = false;
    // The control whose interaction is waiting on the bot.
    std::string pendingPath;
    // The string select whose option list is open, and the values chosen so far in it.
    std::string openSelectPath;
    const std::vector<std::string> *openSelectValues = nullptr;
};

// Uses the visible (possibly forwarded) message; markdown returns trusted RML.
[[nodiscard]] std::string renderMessageComponents(const Discord::Message &message,
                                                const ComponentRenderContext &context);

// The component a rendered data-component-path names, in the visible message; null for
// gallery items or a path the message no longer has.
[[nodiscard]] const Discord::MessageComponent *componentAtPath(const Discord::Message &message,
                                                             std::string_view path);

// Discord's min/max values for a string select, bounded by its option count.
[[nodiscard]] SelectLimits stringSelectLimits(const Discord::MessageComponent &select);

} // namespace Styx::UI::Rml
