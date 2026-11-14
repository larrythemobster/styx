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

TEST_CASE("A multi-factor requirement without a usable challenge keeps Discord's code", "[account]")
{
    const auto mfa = passwordChangeResult(response(401, R"({"code":60003,"message":"Two factor is required for this operation"})"), self);
    REQUIRE_FALSE(mfa.success());
    CHECK(mfa.code == MfaRequiredCode);
}

TEST_CASE("Authenticator secrets are unpadded base32", "[account]")
{
    CHECK(totpSecretFromBytes({'f', 'o', 'o', 'b', 'a', 'r'}) == "MZXW6YTBOI");
    const auto secret = totpSecretFromBytes(std::vector<unsigned char>(20, 0xFF));
    CHECK(secret.size() == 32);
    CHECK(secret == std::string(32, '7'));
    CHECK(groupedTotpSecret("ABCDEFGHIJ") == "abcd efgh ij");
}

TEST_CASE("The setup link names the account and escapes it", "[account]")
{
    CHECK(totpUri("MZXW6YTB", "larry_the.mobster") == "otpauth://totp/Discord:larry_the.mobster?secret=MZXW6YTB&issuer=Discord");
    CHECK(totpUri("MZXW6YTB", "a b&c") == "otpauth://totp/Discord:a%20b%26c?secret=MZXW6YTB&issuer=Discord");
}

TEST_CASE("Authenticator codes are six digits, spaces allowed", "[account]")
{
    CHECK(normalizedTotpCode("123 456") == "123456");
    CHECK(normalizedTotpCode("12345").empty());
    CHECK(normalizedTotpCode("12345a").empty());
    CHECK(normalizedTotpCode("1234567").empty());
}

TEST_CASE("Setting up the authenticator first checks only the password", "[account]")
{
    CHECK(totpEnableBody("pw") == nlohmann::json{{"password", "pw"}});
    CHECK(totpEnableBody("pw", "SECRET", "123456") == nlohmann::json{{"password", "pw"}, {"secret", "SECRET"}, {"code", "123456"}});
    CHECK(totpPasswordCheckResult(response(400, R"({"code":60005,"message":"Invalid two-factor secret"})")).success());
    const auto wrong = totpPasswordCheckResult(response(400,
        R"({"code":50035,"errors":{"password":{"_errors":[{"code":"PASSWORD_DOES_NOT_MATCH","message":"Password does not match."}]}}})"));
    REQUIRE_FALSE(wrong.success());
    CHECK(wrong.error == "Password does not match.");
}

TEST_CASE("Enabling the authenticator returns the new token and backup codes", "[account]")
{
    const auto enabled = totpEnableResult(response(200,
        R"({"token":"MTIzNDU2Nzg5MDEyMzQ1Njc4.abc.def","backup_codes":[{"user_id":"1","code":"zqs8oqxk","consumed":false},{"code":"abcd1234","consumed":true}]})"), self);
    REQUIRE(enabled.success());
    CHECK(enabled.value->token == "MTIzNDU2Nzg5MDEyMzQ1Njc4.abc.def");
    REQUIRE(enabled.value->backupCodes.size() == 2);
    CHECK(enabled.value->backupCodes[0].code == "zqs8oqxk");
    CHECK_FALSE(enabled.value->backupCodes[0].consumed);
    CHECK(enabled.value->backupCodes[1].consumed);
    CHECK_FALSE(totpEnableResult(response(200, R"({"token":"OTk5OTk5OTk5OTk5OTk5OTk5.abc.def"})"), self).success());
    CHECK_FALSE(totpEnableResult(response(400, R"({"code":60008,"message":"Invalid two-factor code"})"), self).success());
}

TEST_CASE("Actions needing a second factor expose Discord's challenge", "[account]")
{
    const auto challenge = mfaChallenge(response(401,
        R"({"code":60003,"message":"Two factor is required for this operation","mfa":{"ticket":"T1","methods":[{"type":"webauthn","challenge":"{}"},{"type":"totp","backup_codes_allowed":true}]}})"));
    REQUIRE(challenge);
    CHECK(challenge->ticket == "T1");
    CHECK(challenge->totp);
    CHECK(challenge->backup);
    CHECK_FALSE(challenge->password);
    // Accounts without a second factor confirm with their password.
    const auto passwordOnly = mfaChallenge(response(401, R"({"code":60003,"mfa":{"ticket":"T2","methods":[{"type":"password"}]}})"));
    REQUIRE(passwordOnly);
    CHECK(passwordOnly->password);
    CHECK_FALSE(passwordOnly->totp);
    CHECK_FALSE(mfaChallenge(response(401, R"({"code":60003,"message":"no ticket"})")));
    CHECK_FALSE(mfaChallenge(response(400, R"({"code":50035})")));
    CHECK_FALSE(mfaChallenge(response(200, R"({"token":"x"})")));
}

TEST_CASE("Second-factor codes pick their method by shape", "[account]")
{
    CHECK(mfaFinishBody("T", mfaCodeProof("123 456")) == nlohmann::json{{"ticket", "T"}, {"mfa_type", "totp"}, {"data", "123456"}});
    CHECK(mfaFinishBody("T", mfaCodeProof("ZQS8-OQXK")) == nlohmann::json{{"ticket", "T"}, {"mfa_type", "backup"}, {"data", "zqs8oqxk"}});
    CHECK(mfaFinishBody("T", mfaPasswordProof("Pass Word")) == nlohmann::json{{"ticket", "T"}, {"mfa_type", "password"}, {"data", "Pass Word"}});
    const auto verified = mfaFinishResult(response(200, R"({"token":"jwt"})"));
    REQUIRE(verified.success());
    CHECK(*verified.value == "jwt");
    CHECK_FALSE(mfaFinishResult(response(400, R"({"code":60008,"message":"Invalid two-factor code"})")).success());
}

TEST_CASE("Removing the authenticator returns this account's new token", "[account]")
{
    CHECK(*totpDisableResult(response(200, R"({"token":"MTIzNDU2Nzg5MDEyMzQ1Njc4.abc.def"})"), self).value == "MTIzNDU2Nzg5MDEyMzQ1Njc4.abc.def");
    CHECK_FALSE(totpDisableResult(response(200, R"({})"), self).success());
}

TEST_CASE("Viewing backup codes needs both of Discord's nonces and returns the codes", "[account]")
{
    const auto nonces = backupCodesChallengeResult(response(200, R"({"nonce":"N1","regenerate_nonce":"N2"})"));
    REQUIRE(nonces.success());
    CHECK(nonces.value->view == "N1");
    CHECK(nonces.value->regenerate == "N2");
    CHECK_FALSE(backupCodesChallengeResult(response(200, R"({"nonce":"N1"})")).success());
    const auto wrong = backupCodesChallengeResult(response(400,
        R"({"code":50035,"errors":{"password":{"_errors":[{"code":"PASSWORD_DOES_NOT_MATCH","message":"Password does not match."}]}}})"));
    REQUIRE_FALSE(wrong.success());
    CHECK(wrong.error == "Password does not match.");

    CHECK(backupCodesBody("key1", "N2", true) == nlohmann::json::parse(R"({"key":"key1","nonce":"N2","regenerate":true})"));
    const auto codes = backupCodesResult(response(200, R"({"backup_codes":[{"user_id":"1","code":"zqs8oqxk","consumed":true},{"code":7}]})"));
    REQUIRE(codes.success());
    REQUIRE(codes.value->size() == 1);
    CHECK(codes.value->front().code == "zqs8oqxk");
    CHECK(codes.value->front().consumed);
    CHECK_FALSE(backupCodesResult(response(200, R"({"backup_codes":[]})")).success());
    CHECK(backupCodesResult(response(400, R"({"code":50035,"message":"Invalid verification key"})")).code == 50035);
}
