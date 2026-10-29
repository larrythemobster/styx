#include "ContextProperties.hpp"
#include "Platform/Base64.hpp"

namespace Styx::Discord {
ContextProperties ContextProperties::empty() { return {}; }
ContextProperties ContextProperties::location(const std::string &location)
{
    return ContextProperties().add("location", location);
}
ContextProperties &ContextProperties::add(const std::string &key, const nlohmann::json &value)
{
    json.insert(key, value);
    return *this;
}
std::string ContextProperties::toHeaderValue() const
{
    return Platform::toBase64(json.toBytes());
}
}
