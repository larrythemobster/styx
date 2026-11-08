#include <catch2/catch_test_macros.hpp>

#include "Core/NotificationPolicy.hpp"

using namespace Styx::Core;
using Level = Styx::Discord::MessageNotificationLevel;

static NotificationFacts guildMessage(Level level)
{
    NotificationFacts facts;
    facts.privateChannel = false;
    facts.level = level;
    return facts;
}

TEST_CASE("Own messages never notify", "[notifications]")
{
    NotificationFacts dm;
    dm.privateChannel = true;
    dm.fromSelf = true;
    REQUIRE_FALSE(shouldNotify(dm));

    NotificationFacts guild = guildMessage(Level::ALL_MESSAGES);
    guild.fromSelf = true;
    guild.mentionsMe = true;
    REQUIRE_FALSE(shouldNotify(guild));
}

TEST_CASE("Blocked authors never notify", "[notifications]")
{
    NotificationFacts dm;
    dm.privateChannel = true;
    dm.authorBlocked = true;
    REQUIRE_FALSE(shouldNotify(dm));

    NotificationFacts guild = guildMessage(Level::ONLY_MENTIONS);
    guild.mentionsMe = true;
    guild.authorBlocked = true;
    REQUIRE_FALSE(shouldNotify(guild));
}

TEST_CASE("The open channel in a focused window doesn't notify", "[notifications]")
{
    NotificationFacts dm;
    dm.privateChannel = true;
    dm.viewingChannel = true;
    REQUIRE_FALSE(shouldNotify(dm));
    dm.viewingChannel = false;
    REQUIRE(shouldNotify(dm));

    NotificationFacts guild = guildMessage(Level::ONLY_MENTIONS);
    guild.mentionsMe = true;
    guild.viewingChannel = true;
    REQUIRE_FALSE(shouldNotify(guild));
    guild.viewingChannel = false;
    REQUIRE(shouldNotify(guild));
}

TEST_CASE("Mute silences everything, mentions included", "[notifications]")
{
    NotificationFacts dm;
    dm.privateChannel = true;
    dm.muted = true;
    REQUIRE_FALSE(shouldNotify(dm));

    NotificationFacts guildAll = guildMessage(Level::ALL_MESSAGES);
    guildAll.muted = true;
    REQUIRE_FALSE(shouldNotify(guildAll));

    NotificationFacts guildMentions = guildMessage(Level::ONLY_MENTIONS);
    guildMentions.mentionsMe = true;
    guildMentions.muted = true;
    REQUIRE_FALSE(shouldNotify(guildMentions));
}

TEST_CASE("DMs notify regardless of level", "[notifications]")
{
    NotificationFacts dm;
    dm.privateChannel = true;
    dm.level = Level::NO_MESSAGES;
    REQUIRE(shouldNotify(dm));
}

TEST_CASE("All messages level notifies for plain messages", "[notifications]")
{
    REQUIRE(shouldNotify(guildMessage(Level::ALL_MESSAGES)));
    REQUIRE(shouldNotify(guildMessage(Level::INHERIT)));
}

TEST_CASE("Nothing level ignores mentions", "[notifications]")
{
    NotificationFacts guild = guildMessage(Level::NO_MESSAGES);
    guild.mentionsMe = true;
    REQUIRE_FALSE(shouldNotify(guild));
}

TEST_CASE("Only mentions level", "[notifications]")
{
    NotificationFacts plain = guildMessage(Level::ONLY_MENTIONS);
    REQUIRE_FALSE(shouldNotify(plain));

    NotificationFacts mentionsMe = guildMessage(Level::ONLY_MENTIONS);
    mentionsMe.mentionsMe = true;
    REQUIRE(shouldNotify(mentionsMe));

    NotificationFacts mentionsEveryone = guildMessage(Level::ONLY_MENTIONS);
    mentionsEveryone.mentionsEveryone = true;
    REQUIRE(shouldNotify(mentionsEveryone));

    NotificationFacts suppressEveryone = mentionsEveryone;
    suppressEveryone.suppressEveryone = true;
    REQUIRE_FALSE(shouldNotify(suppressEveryone));

    NotificationFacts mentionsRole = guildMessage(Level::ONLY_MENTIONS);
    mentionsRole.mentionsMyRole = true;
    REQUIRE(shouldNotify(mentionsRole));

    NotificationFacts suppressRole = mentionsRole;
    suppressRole.suppressRoles = true;
    REQUIRE_FALSE(shouldNotify(suppressRole));

    NotificationFacts mentionsMeSuppressed = guildMessage(Level::ONLY_MENTIONS);
    mentionsMeSuppressed.mentionsMe = true;
    mentionsMeSuppressed.suppressEveryone = true;
    mentionsMeSuppressed.suppressRoles = true;
    REQUIRE(shouldNotify(mentionsMeSuppressed));
}
