#include <catch2/catch_test_macros.hpp>

#include "Core/AccountDetails.hpp"

using namespace Styx::Core;

TEST_CASE("Email addresses hide the name and keep the domain", "[account]")
{
    CHECK(maskEmail("larry.mobster@gmail.com") == "*************@gmail.com");
    CHECK(maskEmail("a@b.co") == "*@b.co");
    // Only the last @ separates the domain.
    CHECK(maskEmail("\"x@y\"@example.org") == "*****@example.org");
}

TEST_CASE("Malformed email addresses are hidden completely", "[account]")
{
    CHECK(maskEmail("not-an-address") == "**************");
    CHECK(maskEmail("") == "");
}

TEST_CASE("Non-ASCII email names mask one asterisk per character", "[account]")
{
    CHECK(maskEmail("j\xC3\xBCrgen@example.de") == "******@example.de");
}

TEST_CASE("Phone numbers keep only their last four characters", "[account]")
{
    CHECK(maskPhone("+14155551629") == "********1629");
    CHECK(maskPhone("+4412345") == "****2345");
}

TEST_CASE("Short phone numbers are hidden completely", "[account]")
{
    CHECK(maskPhone("1629") == "****");
    CHECK(maskPhone("12") == "**");
    CHECK(maskPhone("") == "");
}
