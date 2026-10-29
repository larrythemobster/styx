#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "Core/OrderedJson.hpp"

namespace Styx::Discord {
class ContextProperties
{
public:
    static ContextProperties empty();
    static ContextProperties location(const std::string &location);
    ContextProperties &add(const std::string &key, const nlohmann::json &value);
    std::string toHeaderValue() const;
private:
    Core::OrderedJson json;
};
}
