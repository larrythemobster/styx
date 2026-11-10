#pragma once

#include "Core/JsonUtils.hpp"
#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Entities.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Styx::Discord {

struct HttpResponse;

struct GroupDmEdit : Core::JsonUtils::JsonObject
{
    Field<std::string, true> name;
    Field<std::string, true, true> icon;

    [[nodiscard]] Core::OrderedJson toJson() const;
};

[[nodiscard]] Core::Result<nlohmann::json> groupDmCreateBody(const std::vector<Core::Snowflake> &recipients);
[[nodiscard]] Core::Result<Channel> groupDmChannelResult(const HttpResponse &response,
                                                        std::optional<Core::Snowflake> expectedChannelId = std::nullopt);
[[nodiscard]] Core::Result<std::optional<Channel>> addDmRecipientResult(const HttpResponse &response);
[[nodiscard]] Core::Result<void> groupDmActionResult(const HttpResponse &response);

} // namespace Styx::Discord
