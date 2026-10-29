#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Core {

class OrderedJson
{
public:
    class Array {
    public:
        Array &append(const nlohmann::json &value);
        Array &append(const OrderedJson &object);
        Array &append(const Array &array);
        std::string toBytes() const;
    private:
        std::vector<std::string> elements;
    };
    OrderedJson &insert(std::string_view key, const nlohmann::json &value);
    OrderedJson &insert(std::string_view key, const OrderedJson &object);
    OrderedJson &insert(std::string_view key, const Array &array);
    std::string toBytes() const;
private:
    OrderedJson &insertRaw(std::string_view key, std::string json);
    std::vector<std::string> members;
};

} // namespace Styx::Core
