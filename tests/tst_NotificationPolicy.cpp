#include <catch2/catch_test_macros.hpp>

#include "Core/NotificationPolicy.hpp"
#include "Core/NotificationText.hpp"

#include <string>
#include <string_view>

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

TEST_CASE("A notified message counts as a mention only when it mentions the user", "[notifications]")
{
    NotificationFacts plain = guildMessage(Level::ALL_MESSAGES);
    REQUIRE(shouldNotify(plain));
    REQUIRE_FALSE(isMentioned(plain));

    NotificationFacts everyone = plain;
    everyone.mentionsEveryone = true;
    REQUIRE(isMentioned(everyone));
    everyone.suppressEveryone = true;
    REQUIRE(shouldNotify(everyone));
    REQUIRE_FALSE(isMentioned(everyone));
}

TEST_CASE("Do Not Disturb silences everything, DMs and mentions included", "[notifications]")
{
    NotificationFacts dm;
    dm.privateChannel = true;
    dm.doNotDisturb = true;
    REQUIRE_FALSE(shouldNotify(dm));

    NotificationFacts mention = guildMessage(Level::ALL_MESSAGES);
    mention.mentionsMe = true;
    mention.doNotDisturb = true;
    REQUIRE_FALSE(shouldNotify(mention));
    mention.doNotDisturb = false;
    REQUIRE(shouldNotify(mention));
}

static MentionNames sampleNames()
{
    MentionNames names;
    names.user = [](std::string_view id) { return id == "1" ? std::string("Ann") : std::string(); };
    names.channel = [](std::string_view id) { return id == "2" ? std::string("general") : std::string(); };
    names.role = [](std::string_view id) { return id == "3" ? std::string("Mods") : std::string(); };
    return names;
}

TEST_CASE("Mentions are spelled out", "[notifications]")
{
    REQUIRE(notificationText("hi <@1> and <@!1>", 0, 0, sampleNames()) == "hi @Ann and @Ann");
    REQUIRE(notificationText("<@&3> look", 0, 0, sampleNames()) == "@Mods look");
    REQUIRE(notificationText("see <#2>", 0, 0, sampleNames()) == "see #general");
    REQUIRE(notificationText("<@9> <@&9> <#9>", 0, 0, sampleNames()) == "@unknown-user @deleted-role #deleted-channel");
}

TEST_CASE("Malformed tags stay as typed", "[notifications]")
{
    REQUIRE(notificationText("a <@x> b", 0, 0, sampleNames()) == "a <@x> b");
    REQUIRE(notificationText("<#12", 0, 0, sampleNames()) == "<#12");
    REQUIRE(notificationText("1 < 2", 0, 0, sampleNames()) == "1 < 2");
}

TEST_CASE("Custom emoji and timestamps", "[notifications]")
{
    REQUIRE(notificationText("nice <:pog:123> <a:dance:45>", 0, 0, sampleNames()) == "nice :pog: :dance:");
    REQUIRE(notificationText("at <t:1700000000:R>", 0, 0, sampleNames()) == "at 1700000000");
}

TEST_CASE("Spoilers are hidden", "[notifications]")
{
    REQUIRE(notificationText("it was ||the butler|| all along", 0, 0, sampleNames()) == "it was (spoiler) all along");
    REQUIRE(notificationText("a || b", 0, 0, sampleNames()) == "a || b");
}

TEST_CASE("Markdown markers are removed but single stars kept", "[notifications]")
{
    REQUIRE(notificationText("**bold** __under__ ~~gone~~ `code`", 0, 0, sampleNames()) == "bold under gone code");
    REQUIRE(notificationText("2*3 snake_case", 0, 0, sampleNames()) == "2*3 snake_case");
}

TEST_CASE("Empty text describes attachments and stickers", "[notifications]")
{
    REQUIRE(notificationText("", 1, 0, sampleNames()) == "Sent an attachment");
    REQUIRE(notificationText("  ", 3, 0, sampleNames()) == "Sent 3 attachments");
    REQUIRE(notificationText("", 0, 1, sampleNames()) == "Sent a sticker");
    REQUIRE(notificationText("", 0, 0, sampleNames()) == "");
    REQUIRE(notificationText("look", 2, 0, sampleNames()) == "look");
}

TEST_CASE("Long text is cut on a character boundary", "[notifications]")
{
    std::string text(199, 'a');
    text += "\xC3\xA9";
    text += "tail";
    REQUIRE(notificationText(text, 0, 0, sampleNames()) == std::string(199, 'a') + "\xE2\x80\xA6");

    std::string longText(300, 'a');
    REQUIRE(notificationText(longText, 0, 0, sampleNames()) == std::string(200, 'a') + "\xE2\x80\xA6");
}

TEST_CASE("Missing lookups fall back like unknown names", "[notifications]")
{
    REQUIRE(notificationText("<@1>", 0, 0, MentionNames{}) == "@unknown-user");
}
