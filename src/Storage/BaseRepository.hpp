#pragma once

#include <string>
#include <optional>
#include <type_traits>

#include "DatabaseManager.hpp"
#include "Platform/Sqlite.hpp"
#include "Core/Logging.hpp"
#include "Core/JsonUtils.hpp"
#include "Platform/Native.hpp"

namespace Styx {
namespace Storage {

class BaseRepository
{
public:
    explicit BaseRepository(const std::string &connName);

protected:
    Platform::Database &getDb() const;

    static bool execLogged(Platform::Statement &stmt, const char *context);

    // Helpers to bind optional fields to named parameters
    template <typename F>
    static void bindOptional(Platform::Statement &stmt, std::string_view name, const F &field)
    {
        if (field.hasValue()) {
            bindValue(stmt, name, field.get());
        } else {
            stmt.bindNull(name);
        }
    }

    template <typename T>
    static void bindOptional(Platform::Statement &stmt, std::string_view name, const std::optional<T> &opt)
    {
        if (opt.has_value()) {
            bindValue(stmt, name, *opt);
        } else {
            stmt.bindNull(name);
        }
    }

    static void bindValue(Platform::Statement &stmt, std::string_view name, int val)
    {
        stmt.bind(name, val);
    }

    static void bindValue(Platform::Statement &stmt, std::string_view name, unsigned int val)
    {
        stmt.bind(name, static_cast<int64_t>(val));
    }

    static void bindValue(Platform::Statement &stmt, std::string_view name, int64_t val)
    {
        stmt.bind(name, val);
    }

    static void bindValue(Platform::Statement &stmt, std::string_view name, uint64_t val)
    {
        stmt.bind(name, static_cast<int64_t>(val));
    }

    static void bindValue(Platform::Statement &stmt, std::string_view name, double val)
    {
        stmt.bind(name, val);
    }


    static void bindValue(Platform::Statement &stmt, std::string_view name, const Platform::DateTime &val)
    {
        std::string s = val.iso8601();
        stmt.bind(name, std::string_view(s));
    }

    template <typename SubT, bool Opt, bool Nullable>
    static void bindValue(Platform::Statement &stmt, std::string_view name, const Core::JsonUtils::JsonObject::Field<SubT, Opt, Nullable> &field)
    {
        if (field.hasValue())
            bindValue(stmt, name, field.get());
        else
            stmt.bindNull(name);
    }

    static void bindValue(Platform::Statement &stmt, std::string_view name, const std::string &val)
    {
        stmt.bind(name, std::string_view(val));
    }

    static void bindValue(Platform::Statement &stmt, std::string_view name, std::string_view val)
    {
        stmt.bind(name, val);
    }

    static void bindValue(Platform::Statement &stmt, std::string_view name, const char *val)
    {
        if (val)
            stmt.bind(name, std::string_view(val));
        else
            stmt.bindNull(name);
    }

    static void bindValue(Platform::Statement &stmt, std::string_view name, bool val)
    {
        stmt.bind(name, val ? 1 : 0);
    }

    static void bindValue(Platform::Statement &stmt, std::string_view name, Core::Snowflake val)
    {
        stmt.bind(name, static_cast<int64_t>(static_cast<uint64_t>(val)));
    }

    std::string connName;
};

} // namespace Storage
} // namespace Styx
