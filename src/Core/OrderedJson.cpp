#include "OrderedJson.hpp"

#include <utility>

namespace Styx::Core {
namespace {
std::string join(const std::vector<std::string> &parts, char open, char close)
{
    std::size_t size = 2 + (parts.empty() ? 0 : parts.size() - 1);
    for (const auto &part : parts) size += part.size();
    std::string result;
    result.reserve(size);
    result += open;
    bool first = true;
    for (const auto &part : parts) {
        if (!first) result += ',';
        first = false;
        result += part;
    }
    result += close;
    return result;
}
}

OrderedJson::Array &OrderedJson::Array::append(const nlohmann::json &value) { elements.push_back(value.dump()); return *this; }
OrderedJson::Array &OrderedJson::Array::append(const OrderedJson &object) { elements.push_back(object.toBytes()); return *this; }
OrderedJson::Array &OrderedJson::Array::append(const Array &array) { elements.push_back(array.toBytes()); return *this; }
std::string OrderedJson::Array::toBytes() const { return join(elements, '[', ']'); }
OrderedJson &OrderedJson::insert(std::string_view key, const nlohmann::json &value) { return insertRaw(key, value.dump()); }
OrderedJson &OrderedJson::insert(std::string_view key, const OrderedJson &object) { return insertRaw(key, object.toBytes()); }
OrderedJson &OrderedJson::insert(std::string_view key, const Array &array) { return insertRaw(key, array.toBytes()); }
OrderedJson &OrderedJson::insertRaw(std::string_view key, std::string json)
{
    members.push_back(nlohmann::json(std::string(key)).dump() + ':' + std::move(json));
    return *this;
}
std::string OrderedJson::toBytes() const { return join(members, '{', '}'); }
} // namespace Styx::Core
