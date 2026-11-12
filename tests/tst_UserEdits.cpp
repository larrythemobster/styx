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

TEST_CASE("Server mute duration and unmute preserve untouched notification preferences", "[user-edits]")
{
    UserGuildSettingsEdit edit;
    MuteConfig config;
    config.endTime = "2026-11-12T15:30:00Z";
    config.selectedTimeWindow = 900;
    edit.muted = true;
    edit.muteConfig = config;
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json{
        {"muted", true},
        {"mute_config", {{"end_time", "2026-11-12T15:30:00Z"}, {"selected_time_window", 900}}}
    });
    config.endTime = nullptr;
    config.selectedTimeWindow = -1;
    edit.muteConfig = config;
    REQUIRE(parseJson(edit.toJson()).at("mute_config") == nlohmann::json{
        {"end_time", nullptr}, {"selected_time_window", -1}
    });
    edit.muted = false;
    edit.muteConfig = nullptr;
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json{{"muted", false}, {"mute_config", nullptr}});
}

TEST_CASE("Server profile removals do not overwrite the account profile", "[user-edits]")
{
    CurrentMemberEdit edit;
    edit.nick = "Server name";
    edit.pronouns = "they/them";
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json{{"nick", "Server name"}, {"pronouns", "they/them"}});
    edit.nick = nullptr;
    edit.avatar = nullptr;
    edit.bio = nullptr;
    REQUIRE(parseJson(edit.toJson()) == nlohmann::json{
        {"nick", nullptr}, {"avatar", nullptr}, {"bio", nullptr}, {"pronouns", "they/them"}
    });
}
