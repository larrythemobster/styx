#pragma once

#include <string>

#include "Result.hpp"
#include "Snowflake.hpp"

namespace Styx {
namespace Core {

class TokenStore
{
public:
    static constexpr char const *SERVICE_NAME = "Styx";

    static Result<void> saveToken(Snowflake accountId, const std::string &token);
    static Result<std::string> loadToken(Snowflake accountId);
    static bool deleteToken(Snowflake accountId);

private:
    static std::string keyForAccount(Snowflake accountId);
    static std::string describeKeychainFailure(const std::string &keychainError);
};

} // namespace Core
} // namespace Styx
