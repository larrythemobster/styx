#pragma once

#include "Objects.hpp"

namespace Styx {
namespace Discord {

struct Inbound
{
    OpCode opcode;
    nlohmann::json data;

    std::optional<int> s;
    std::optional<std::string> t;

    static Inbound fromJson(const nlohmann::json &root)
    {
        Inbound obj;
        obj.opcode = static_cast<OpCode>(root.value("op", 0));
        obj.data = root["d"];

        if (root.contains("s") && !root["s"].is_null())
            obj.s = root["s"].get<int>();

        if (root.contains("t") && !root["t"].is_null())
            obj.t = root["t"].get<std::string>();

        return obj;
    }

    template <typename T>
    T getData() const
    {
        return T::fromJson(data.is_object() ? data : nlohmann::json::object());
    }
};

struct Hello
{
    int heartbeatInterval;

    static Hello fromJson(const nlohmann::json &root)
    {
        Hello obj;
        obj.heartbeatInterval = root.value("heartbeat_interval", 0);
        return obj;
    }
};

} // namespace Discord
} // namespace Styx
