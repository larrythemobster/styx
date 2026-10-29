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
std::string serialize(const nlohmann::json &value)
{
    if (value.is_array()) {
        std::vector<std::string> elements;
        elements.reserve(value.size());
        for (const auto &entry : value) elements.push_back(serialize(entry));
        return join(elements, '[', ']');
    }
    if (value.is_object()) {
        std::vector<std::string> members;
        members.reserve(value.size());
        for (auto it = value.begin(); it != value.end(); ++it)
            members.push_back(nlohmann::json(it.key()).dump() + ':' + serialize(it.value()));
        return join(members, '{', '}');
    }
    if (value.is_number_float()) {
        if (value.get<double>() == 0) return "0";
        auto text = value.dump();
        auto exponent = text.find_first_of("eE");
        auto end = exponent == std::string::npos ? text.size() : exponent;
        if (end >= 2 && text.compare(end - 2, 2, ".0") == 0)
            text.erase(end - 2, 2);
        return text;
    }
    return value.dump();
}
}

OrderedJson::Array &OrderedJson::Array::append(const nlohmann::json &value) { elements.push_back(serialize(value)); return *this; }
OrderedJson::Array &OrderedJson::Array::append(const OrderedJson &object) { elements.push_back(object.toBytes()); return *this; }
OrderedJson::Array &OrderedJson::Array::append(const Array &array) { elements.push_back(array.toBytes()); return *this; }
std::string OrderedJson::Array::toBytes() const { return join(elements, '[', ']'); }
OrderedJson &OrderedJson::insert(std::string_view key, const nlohmann::json &value) { return insertRaw(key, serialize(value)); }
OrderedJson &OrderedJson::insert(std::string_view key, const OrderedJson &object) { return insertRaw(key, object.toBytes()); }
OrderedJson &OrderedJson::insert(std::string_view key, const Array &array) { return insertRaw(key, array.toBytes()); }
OrderedJson &OrderedJson::insertRaw(std::string_view key, std::string json)
{
    members.push_back(nlohmann::json(std::string(key)).dump() + ':' + std::move(json));
    return *this;
}
std::string OrderedJson::toBytes() const { return join(members, '{', '}'); }
} // namespace Styx::Core
