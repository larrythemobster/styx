#pragma once

#include "Core/Result.hpp"
#include "Discord/Entities.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Styx::Discord {

struct PollCreateAnswer
{
    std::string text;
    std::optional<Emoji> emoji;
};

struct PollCreateRequest
{
    std::string question;
    std::vector<PollCreateAnswer> answers;
    int durationHours = 1;
    bool allowMultiselect = false;
};

[[nodiscard]] Core::Result<nlohmann::json> pollCreateBody(const PollCreateRequest &request);

} // namespace Styx::Discord
