#include <catch2/catch_test_macros.hpp>
#include "UI/Rml/ReleaseVersion.hpp"
using namespace Styx::UI::Rml;

TEST_CASE("Release versions follow semantic version precedence", "[release-version]") {
    auto cmp = [](const char *a, const char *b) {
        auto x = parseReleaseVersion(a), y = parseReleaseVersion(b);
        REQUIRE(x); REQUIRE(y);
        return compareReleaseVersions(*x, *y);
    };
    REQUIRE(cmp("v1.10.0", "1.9.99") > 0);
    REQUIRE(cmp("1.0.0-alpha", "1.0.0-alpha.1") < 0);
    REQUIRE(cmp("1.0.0-alpha.2", "1.0.0-alpha.10") < 0);
    REQUIRE(cmp("1.0.0-1", "1.0.0-alpha") < 0);
    REQUIRE(cmp("1.0.0-rc.1", "1.0.0") < 0);
    REQUIRE(cmp("1.0.0+one", "v1.0.0+two") == 0);
    REQUIRE(cmp("99999999999999999999.0.0", "9.0.0") > 0);
}
TEST_CASE("Invalid release tags are not treated as versions", "[release-version]") {
    for (const auto *tag : {"", "v", "1.2", "1.02.3", "1.2.3-", "1.2.3-01", "1.2.3+", "1.2.3+a..b", "1.2.3+bad+metadata", "1.2.3/evil"})
        REQUIRE_FALSE(parseReleaseVersion(tag));
}
