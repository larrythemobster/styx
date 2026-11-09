#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Native.hpp"
#include "UI/Chat/SelectCandidates.hpp"
#include "UI/Chat/SelectDraft.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Styx::Discord { struct Message; struct MessageComponent; }

namespace Styx::UI::Rml {

// All callbacks are required; the pointers may be null.
struct ComponentRenderContext {
    std::function<std::string(const std::string &)> markdown;
    std::function<std::string(const std::string &, Platform::Size)> imageSource;
    std::function<std::string(Core::Snowflake, const std::string &)> defaultLabel;
    std::function<std::string(int64_t)> fileSize;
    const std::unordered_set<std::string> *revealedSpoilers = nullptr;
    // Buttons and selects with a custom_id can be used; forwarded messages stay read-only.
    bool interactive = false;
    // The control whose interaction is waiting on the bot.
    std::string pendingPath;
    // The select whose list is open, and the values chosen so far in it.
    std::string openSelectPath;
    const std::vector<std::string> *openSelectValues = nullptr;
    // For an open user, role, mentionable or channel select: the search text, the
    // candidates matching it, and the chosen candidates for the summary.
    std::string openSelectQuery;
    const std::vector<SelectCandidate> *openSelectCandidates = nullptr;
    const std::vector<SelectCandidate> *openSelectChosen = nullptr;
};

// Uses the visible (possibly forwarded) message; markdown returns trusted RML.
[[nodiscard]] std::string renderMessageComponents(const Discord::Message &message,
                                                const ComponentRenderContext &context);

// The component a rendered data-component-path names, in the visible message; null for
// gallery items or a path the message no longer has.
[[nodiscard]] const Discord::MessageComponent *componentAtPath(const Discord::Message &message,
                                                             std::string_view path);

// Discord's min/max values for a select: string selects are bounded by their option
// count, the others by Discord's 25.
[[nodiscard]] SelectLimits selectComponentLimits(const Discord::MessageComponent &select);

// The candidate rows of an open entity select, also used to refresh them while typing.
[[nodiscard]] std::string renderSelectCandidates(const std::string &path,
                                                 const std::vector<SelectCandidate> &candidates,
                                                 const std::vector<std::string> &chosenValues);

} // namespace Styx::UI::Rml
