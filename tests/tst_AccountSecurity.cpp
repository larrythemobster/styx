#include <catch2/catch_test_macros.hpp>

#include "Discord/AccountSecurity.hpp"
#include "Discord/HttpClient.hpp"

using namespace Styx;
using namespace Styx::Discord;

namespace {
const Core::Snowflake self(123456789012345678ULL);

HttpResponse response(int status, std::string body)
{
    HttpResponse r;
    r.statusCode = status;
    r.success = status >= 200 && status < 300;
    r.body = std::move(body);
    return r;
}
}

TEST_CASE("Password changes are checked before sending", "[account]")
{
    CHECK(passwordChangeProblem("", "longenough", "longenough") == "Enter your current password.");
    CHECK(passwordChangeProblem("old", "short", "short") == "The new password must be at least 8 characters.");
    CHECK(passwordChangeProblem("old", std::string(73, 'a'), std::string(73, 'a')) == "The new password can't be longer than 72 characters.");
    CHECK(passwordChangeProblem("old", "longenough", "longenougH") == "The new passwords don't match.");
    CHECK(passwordChangeProblem("samesame", "samesame", "samesame") == "The new password is the same as the current one.");
    CHECK_FALSE(passwordChangeProblem("old", "longenough", "longenough").has_value());
    // Length counts characters, not bytes: eight two-byte letters are enough.
    const std::string accented = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    CHECK_FALSE(passwordChangeProblem("old", accented, accented).has_value());
}

TEST_CASE("Password change body carries both passwords", "[account]")
{
    const auto body = passwordChangeBody("current", "replacement");
    CHECK(body.size() == 2);
    CHECK(body["password"] == "current");
    CHECK(body["new_password"] == "replacement");
}

TEST_CASE("A successful password change yields this account's new token", "[account]")
{
    const auto result = passwordChangeResult(response(200, R"({"id":"123456789012345678","username":"x","token":"MTIzNDU2Nzg5MDEyMzQ1Njc4.abc.def"})"), self);
    REQUIRE(result.success());
    CHECK(*result.value == "MTIzNDU2Nzg5MDEyMzQ1Njc4.abc.def");
}

TEST_CASE("A password change response without a usable token is an error", "[account]")
{
    CHECK_FALSE(passwordChangeResult(response(200, R"({"id":"123456789012345678"})"), self).success());
    CHECK_FALSE(passwordChangeResult(response(200, R"({"token":""})"), self).success());
    CHECK_FALSE(passwordChangeResult(response(200, "not json"), self).success());
    // A token for someone else must never replace this account's sign-in.
    CHECK_FALSE(passwordChangeResult(response(200, R"({"token":"OTk5OTk5OTk5OTk5OTk5OTk5.abc.def"})"), self).success());
}

TEST_CASE("Discord's password errors are shown as sent", "[account]")
{
    const auto wrong = passwordChangeResult(response(400,
        R"({"code":50035,"errors":{"password":{"_errors":[{"code":"PASSWORD_DOES_NOT_MATCH","message":"Password does not match."}]}},"message":"Invalid Form Body"})"), self);
    REQUIRE_FALSE(wrong.success());
    CHECK(wrong.error == "Password does not match.");
}

TEST_CASE("Accounts that need multi-factor verification are told to use the official app", "[account]")
{
    const auto mfa = passwordChangeResult(response(401, R"({"code":60003,"message":"Two factor is required for this operation"})"), self);
    REQUIRE_FALSE(mfa.success());
    CHECK(mfa.code == MfaRequiredCode);
    CHECK(mfa.error.find("multi-factor") != std::string::npos);
}
