#include "Discord/UserEdits.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>

using namespace Styx;
using namespace Styx::Discord;

namespace {

nlohmann::json parseJson(const Core::OrderedJson &ordered)
{
    return nlohmann::json::parse(ordered.toBytes());
}

} // namespace

TEST_CASE("Account edits distinguish untouched, removed and replacement images", "[user-edits]")
{
    CurrentUserEdit edit;
    REQUIRE(edit.empty());
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json::object());

    edit.avatar = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUg==";
    REQUIRE_FALSE(edit.empty());
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json{
        {"avatar", "data:image/png;base64,iVBORw0KGgoAAAANSUhEUg=="}
    });

    edit.globalName = nullptr;
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json{
        {"global_name", nullptr},
        {"avatar", "data:image/png;base64,iVBORw0KGgoAAAANSUhEUg=="}
    });

    edit.globalName = "Renée ✨";
    edit.avatar = nullptr;
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json{
        {"global_name", "Renée ✨"},
        {"avatar", nullptr}
    });
}

TEST_CASE("Profile text updates do not clear an untouched banner", "[user-edits]")
{
    UserProfileEdit edit;
    REQUIRE(edit.empty());
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json::object());

    edit.pronouns = "they/them";
    edit.bio = "日本語話者\nSecond line";
    REQUIRE_FALSE(edit.empty());
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json{
        {"pronouns", "they/them"},
        {"bio", "日本語話者\nSecond line"}
    });

    edit.pronouns = "";
    edit.bio = "";
    edit.banner = nullptr;
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json{
        {"pronouns", ""},
        {"bio", ""},
        {"banner", nullptr}
    });
}

TEST_CASE("Image removal alone remains an edit without writing profile text", "[user-edits]")
{
    CurrentUserEdit account;
    account.avatar = nullptr;
    REQUIRE_FALSE(account.empty());
    REQUIRE(parseJson(account.toJson()) == nlohmann::json{{"avatar", nullptr}});

    UserProfileEdit profile;
    profile.banner = nullptr;
    REQUIRE_FALSE(profile.empty());
    REQUIRE(parseJson(profile.toJson()) == nlohmann::json{{"banner", nullptr}});

    profile.banner = "data:image/jpeg;base64,/9j/4AAQSkZJRg==";
    REQUIRE(parseJson(profile.toJson()) == nlohmann::json{
        {"banner", "data:image/jpeg;base64,/9j/4AAQSkZJRg=="}
    });
}
