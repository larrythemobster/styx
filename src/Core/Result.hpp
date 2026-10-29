#pragma once

#include <string>
#include <optional>
#include <utility>

namespace Styx {
namespace Core {

template <typename T>
struct Result
{
    std::optional<T> value;
    std::string error;
    int code = 0;

    bool success() const { return value.has_value(); }

    static Result<T> makeOk(T value)
    {
        return { std::move(value), {} };
    }

    static Result<T> makeError(std::string error, int code = 0)
    {
        return { {}, std::move(error), code };
    }
};

template <>
struct Result<void>
{
    bool ok = false;
    std::string error;
    int code = 0;

    bool success() const { return ok; }

    static Result<void> makeOk() { return { true, {} }; }

    static Result<void> makeError(std::string error, int code = 0) { return { false, std::move(error), code }; }
};

} // namespace Core
} // namespace Styx
