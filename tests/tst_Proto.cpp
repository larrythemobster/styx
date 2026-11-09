#include <catch2/catch_test_macros.hpp>

#include "Proto/FrecencySettings.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/UserSettings.hpp"
#include "Proto/ProtoWriter.hpp"
#include "Platform/Base64.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace Styx::Proto;

namespace {

template<class Bytes> std::string toHex(const Bytes &data)
{
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : data) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

std::vector<uint8_t> fromHex(std::string_view hex)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        std::string byteString(hex.substr(i, 2));
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

} // namespace

TEST_CASE("Proto: Frecency round trip", "[proto]")
{
    FrecencyItem thumbsup;
    thumbsup.totalUses = 7;
    thumbsup.recentUses = { 1700000000000ULL, 1700000060000ULL, 1700000120000ULL };
    thumbsup.frecency = -1;
    thumbsup.score = 270;

    FrecencyItem custom;
    custom.totalUses = 1;
    custom.recentUses = { 1699999999999ULL };
    custom.frecency = 42;
    custom.score = 100;

    FrecencyUserSettings settings;
    EmojiFrecency chat;
    chat.emojis.insert_or_assign("thumbsup", thumbsup);
    chat.emojis.insert_or_assign("123456789012345678", custom);
    settings.emojiFrecency = chat;

    EmojiFrecency reactions;
    reactions.emojis.insert_or_assign("eyes", thumbsup);
    settings.emojiReactionFrecency = reactions;

    settings.dataVersion = 9;

    const auto encoded = settings.toProtoPartial();
    ProtoReader reader(encoded);
    FrecencyUserSettings decoded = FrecencyUserSettings::fromProto(reader);

    // toProtoPartial only carries the two frecency maps
    REQUIRE(!decoded.dataVersion.has_value());

    REQUIRE(decoded.emojiFrecency.has_value());
    REQUIRE(decoded.emojiFrecency->emojis.size() == 2);

    REQUIRE(decoded.emojiFrecency->emojis.contains("thumbsup"));
    const FrecencyItem &decodedThumbsup = decoded.emojiFrecency->emojis.at("thumbsup");
    REQUIRE(decodedThumbsup.totalUses == 7u);
    REQUIRE(decodedThumbsup.recentUses == thumbsup.recentUses);
    REQUIRE(decodedThumbsup.frecency == -1);
    REQUIRE(decodedThumbsup.score == 270);

    REQUIRE(decoded.emojiFrecency->emojis.contains("123456789012345678"));
    const FrecencyItem &decodedCustom = decoded.emojiFrecency->emojis.at("123456789012345678");
    REQUIRE(decodedCustom.totalUses == 1u);
    REQUIRE(decodedCustom.recentUses == custom.recentUses);
    REQUIRE(decodedCustom.frecency == 42);
    REQUIRE(decodedCustom.score == 100);

    REQUIRE(decoded.emojiReactionFrecency.has_value());
    REQUIRE(decoded.emojiReactionFrecency->emojis.size() == 1);
    REQUIRE(decoded.emojiReactionFrecency->emojis.at("eyes").totalUses == 7u);
}

TEST_CASE("Proto: Map encoding", "[proto]")
{
    FrecencyItem item;
    item.totalUses = 1;
    item.recentUses = { 2 };

    FrecencyUserSettings settings;
    EmojiFrecency chat;
    chat.emojis.insert_or_assign("a", item);
    settings.emojiFrecency = chat;

    // 32 0c            field 6 (emoji_frecency), 12 bytes
    //   0a 0a          field 1 (map entry), 10 bytes
    //     0a 01 61     key = "a"
    //     12 05        value, 5 bytes
    //       08 01      total_uses = 1
    //       12 01 02   recent_uses = [2], packed
    REQUIRE(toHex(settings.toProtoPartial()) == "320c0a0a0a016112050801120102");
}

TEST_CASE("Proto: Skips unknown fields", "[proto]")
{
    // 0a 06 08 01 10 02 18 2a          versions { client=1, server=2, data=42 }
    // 38 05                            unknown varint field 7
    // 2a 09 0a 03 61 62 63 0a 02 64 65 favorite_emojis { "abc", "de" } (not decoded)
    // 42 02 ff ff                      unknown length-delimited field 8
    std::vector<uint8_t> fixture = fromHex("0a0608011002182a"
                                           "3805"
                                           "2a090a036162630a026465"
                                           "4202ffff");

    ProtoReader reader(fixture);
    FrecencyUserSettings settings = FrecencyUserSettings::fromProto(reader);

    REQUIRE(settings.dataVersion.has_value());
    REQUIRE(*settings.dataVersion == 42u);

    REQUIRE(!settings.emojiFrecency.has_value());
    REQUIRE(!settings.emojiReactionFrecency.has_value());
}

TEST_CASE("Proto: Negative int32", "[proto]")
{
    ProtoWriter writer;
    writer.writeInt32(3, -1);

    // tag 0x18 then the sign-extended 10 byte varint protobuf uses for int32
    REQUIRE(toHex(writer.bytes()) == "18ffffffffffffffffff01");

    ProtoReader reader(writer.bytes());
    FrecencyItem item = FrecencyItem::fromProto(reader);
    REQUIRE(item.frecency == -1);
}

TEST_CASE("Proto: Favorite GIF encoding", "[proto]")
{
    FavoriteGif gif;
    gif.url = "u";
    gif.format = GifType::Video;
    gif.src = "s";
    gif.width = 3;
    gif.height = 4;
    gif.order = 5;

    FavoriteGifs favorites;
    favorites.gifs.push_back(gif);
    favorites.hideTooltip = true;

    const std::string entryKeyedUThenHideTooltip = "0a100a0175120b08021201731803200428051001";
    REQUIRE(toHex(favorites.toProto()) == entryKeyedUThenHideTooltip);

    const FavoriteGifs decoded = FavoriteGifs::fromProto(favorites.toProto());
    REQUIRE(decoded.gifs.size() == 1);
    REQUIRE(decoded.gifs[0].url == "u");
    REQUIRE(decoded.gifs[0].format == GifType::Video);
    REQUIRE(decoded.gifs[0].src == "s");
    REQUIRE(decoded.gifs[0].width == 3u);
    REQUIRE(decoded.gifs[0].height == 4u);
    REQUIRE(decoded.gifs[0].order == 5u);
    REQUIRE(decoded.hideTooltip);
}

TEST_CASE("Proto: Favorite GIFs keep unknown fields", "[proto]")
{
    const std::string unknownVarintField9 = "4807";
    std::vector<uint8_t> entryFromServer = fromHex("0a120a0175120d0802120173180320042805" + unknownVarintField9);
    std::vector<uint8_t> unknownField = fromHex("1a01ff");
    std::vector<uint8_t> hideTooltip = fromHex("1001");

    std::vector<uint8_t> combined = entryFromServer;
    combined.insert(combined.end(), unknownField.begin(), unknownField.end());
    combined.insert(combined.end(), hideTooltip.begin(), hideTooltip.end());

    FavoriteGifs favorites = FavoriteGifs::fromProto(combined);
    REQUIRE(favorites.gifs.size() == 1);
    REQUIRE(favorites.gifs[0].order == 5u);
    REQUIRE(favorites.hideTooltip);

    FavoriteGif added;
    added.url = "v";
    added.format = GifType::Image;
    added.src = "t";
    added.order = 6;
    favorites.gifs.push_back(added);

    std::vector<uint8_t> entryMadeHere = fromHex("0a0c0a0176120708011201742806");

    std::vector<uint8_t> expected1 = entryFromServer;
    expected1.insert(expected1.end(), entryMadeHere.begin(), entryMadeHere.end());
    expected1.insert(expected1.end(), hideTooltip.begin(), hideTooltip.end());
    expected1.insert(expected1.end(), unknownField.begin(), unknownField.end());

    REQUIRE(toHex(favorites.toProto()) == toHex(expected1));

    favorites.gifs.erase(favorites.gifs.begin());

    std::vector<uint8_t> expected2 = entryMadeHere;
    expected2.insert(expected2.end(), hideTooltip.begin(), hideTooltip.end());
    expected2.insert(expected2.end(), unknownField.begin(), unknownField.end());

    REQUIRE(toHex(favorites.toProto()) == toHex(expected2));
}

TEST_CASE("Proto: Favorite GIFs travel alone in a patch", "[proto]")
{
    FrecencyUserSettings settings;
    settings.dataVersion = 9;
    settings.favoriteGifs = FavoriteGifs();
    REQUIRE(toHex(settings.toProtoPartial()) == "1200");

    std::vector<uint8_t> input = fromHex("12140a100a0175120b08021201731803200428051001");
    ProtoReader reader(input);
    const FrecencyUserSettings decoded = FrecencyUserSettings::fromProto(reader);
    REQUIRE(decoded.favoriteGifs.has_value());
    REQUIRE(decoded.favoriteGifs->gifs.size() == 1);
    REQUIRE(!decoded.emojiFrecency.has_value());
}

TEST_CASE("Proto: Guild and channel frecency round trip", "[proto]")
{
    FrecencyItem guild;
    guild.totalUses = 9;
    guild.recentUses = { 1700000000000ULL, 1700000060000ULL, 1700000120000ULL };
    guild.frecency = -15;
    guild.score = 350;

    FrecencyItem channel;
    channel.totalUses = 4;
    channel.recentUses = { 1700000180000ULL };
    channel.frecency = 50;
    channel.score = 120;

    const uint64_t largeId = 1234567890123456789ULL;
    const uint64_t smallId = 987654321ULL;

    FrecencyUserSettings settings;
    GuildAndChannelFrecency places;
    places.guildAndChannels.insert_or_assign(largeId, guild);
    places.guildAndChannels.insert_or_assign(smallId, channel);
    settings.guildAndChannelFrecency = places;
    settings.dataVersion = 7;

    const auto encoded = settings.toProtoPartial();
    ProtoReader reader(encoded);
    FrecencyUserSettings decoded = FrecencyUserSettings::fromProto(reader);

    REQUIRE_FALSE(decoded.dataVersion.has_value());
    REQUIRE(decoded.guildAndChannelFrecency.has_value());
    REQUIRE(decoded.guildAndChannelFrecency->guildAndChannels.size() == 2);

    REQUIRE(decoded.guildAndChannelFrecency->guildAndChannels.contains(largeId));
    const FrecencyItem &decodedGuild = decoded.guildAndChannelFrecency->guildAndChannels.at(largeId);
    REQUIRE(decodedGuild.totalUses == 9u);
    REQUIRE(decodedGuild.recentUses == guild.recentUses);
    REQUIRE(decodedGuild.frecency == -15);
    REQUIRE(decodedGuild.score == 350);

    REQUIRE(decoded.guildAndChannelFrecency->guildAndChannels.contains(smallId));
    const FrecencyItem &decodedChannel = decoded.guildAndChannelFrecency->guildAndChannels.at(smallId);
    REQUIRE(decodedChannel.totalUses == 4u);
    REQUIRE(decodedChannel.recentUses == channel.recentUses);
    REQUIRE(decodedChannel.frecency == 50);
    REQUIRE(decodedChannel.score == 120);
}

TEST_CASE("Proto: Guild and channel frecency hand-built wire format", "[proto]")
{
    // A fixed64 field is its tag (field 1, wire type 1) and then eight little-endian bytes.
    ProtoWriter key;
    key.writeFixed64(1, 0x0102030405060708ULL);
    REQUIRE(key.bytes() == std::string("\x09\x08\x07\x06\x05\x04\x03\x02\x01", 9));

    const uint64_t id = 1234567890123456789ULL;

    ProtoWriter item;
    item.writeVarint(1, 3);

    ProtoWriter entry;
    entry.writeFixed64(1, id);
    entry.writeBytes(2, item.bytes());

    ProtoWriter field12;
    field12.writeBytes(1, entry.bytes());

    ProtoWriter root;
    root.writeBytes(12, field12.bytes());

    ProtoReader reader(root.bytes());
    FrecencyUserSettings decoded = FrecencyUserSettings::fromProto(reader);

    REQUIRE(decoded.guildAndChannelFrecency.has_value());
    REQUIRE(decoded.guildAndChannelFrecency->guildAndChannels.size() == 1);
    REQUIRE(decoded.guildAndChannelFrecency->guildAndChannels.contains(id));
    REQUIRE(decoded.guildAndChannelFrecency->guildAndChannels.at(id).totalUses == 3u);
}

TEST_CASE("Proto: Guild and channel frecency skips non-fixed64 key", "[proto]")
{
    const uint64_t validId = 1234567890123456789ULL;
    const uint64_t skippedVarintId = 42ULL;

    ProtoWriter item;
    item.writeVarint(1, 3);

    // Malformed entry: key is written as a varint instead of fixed64
    ProtoWriter badEntry;
    badEntry.writeVarint(1, skippedVarintId);
    badEntry.writeBytes(2, item.bytes());

    // Valid entry following the malformed one
    ProtoWriter goodEntry;
    goodEntry.writeFixed64(1, validId);
    goodEntry.writeBytes(2, item.bytes());

    ProtoWriter field12;
    field12.writeBytes(1, badEntry.bytes());
    field12.writeBytes(1, goodEntry.bytes());

    ProtoWriter root;
    root.writeBytes(12, field12.bytes());

    ProtoReader reader(root.bytes());
    FrecencyUserSettings decoded = FrecencyUserSettings::fromProto(reader);

    REQUIRE(decoded.guildAndChannelFrecency.has_value());
    REQUIRE(decoded.guildAndChannelFrecency->guildAndChannels.size() == 1);
    REQUIRE(decoded.guildAndChannelFrecency->guildAndChannels.contains(validId));
    REQUIRE_FALSE(decoded.guildAndChannelFrecency->guildAndChannels.contains(skippedVarintId));
    REQUIRE(decoded.guildAndChannelFrecency->guildAndChannels.at(validId).totalUses == 3u);
}

TEST_CASE("Proto: Custom status patch clear on empty is literal 5a00", "[proto]")
{
    StatusSettings current;
    const std::string patch = customStatusPatch(current, std::nullopt);
    REQUIRE(toHex(patch) == "5a00");

    const std::vector<uint8_t> patchBytes = fromHex("5a00");
    ProtoReader reader(patchBytes);
    const PreloadedUserSettings decoded = PreloadedUserSettings::fromProto(reader);
    REQUIRE(decoded.status.has_value());
    REQUIRE_FALSE(decoded.status->customStatus.has_value());
    REQUIRE_FALSE(decoded.status->status.has_value());
    REQUIRE_FALSE(decoded.status->showCurrentGame.has_value());
    REQUIRE(decoded.status->statusExpiresAtMs == 0u);
    REQUIRE(decoded.status->unknownFields.empty());
    REQUIRE_FALSE(decoded.guildFolders.has_value());
    REQUIRE_FALSE(decoded.localization.has_value());
}

TEST_CASE("Proto: Custom status patch set with unicode emoji matches literal bytes", "[proto]")
{
    StatusSettings current;
    CustomStatus custom;
    custom.text = "Busy";
    custom.emojiName = "\xf0\x9f\x94\xa5";

    const std::string patch = customStatusPatch(current, custom);
    REQUIRE(toHex(patch) == "5a0e120c0a04427573791a04f09f94a5");

    const std::vector<uint8_t> patchBytes = fromHex("5a0e120c0a04427573791a04f09f94a5");
    ProtoReader reader(patchBytes);
    const PreloadedUserSettings decoded = PreloadedUserSettings::fromProto(reader);
    REQUIRE(decoded.status.has_value());
    REQUIRE(decoded.status->customStatus.has_value());
    REQUIRE(decoded.status->customStatus->text == "Busy");
    REQUIRE(decoded.status->customStatus->emojiName == "\xf0\x9f\x94\xa5");
    REQUIRE(decoded.status->customStatus->emojiId == 0u);
    REQUIRE(decoded.status->customStatus->expiresAtMs == 0u);
    REQUIRE(decoded.status->customStatus->createdAtMs == 0u);
    REQUIRE_FALSE(decoded.status->status.has_value());
    REQUIRE_FALSE(decoded.guildFolders.has_value());
    REQUIRE_FALSE(decoded.localization.has_value());
}

TEST_CASE("Proto: Custom status patch clear preserves DND, false showCurrentGame, expiry and unknown fields", "[proto]")
{
    const auto original = fromHex("0a050a03646e6412050a034f6c641a020800210068e5cf8b0100002a02082a98062a");
    const auto current = StatusSettings::fromProto(original);

    const std::string patch = customStatusPatch(current, std::nullopt);
    REQUIRE(toHex(patch) == "5a1b0a050a03646e641a020800210068e5cf8b0100002a02082a98062a");

    const std::vector<uint8_t> patchBytes = fromHex("5a1b0a050a03646e641a020800210068e5cf8b0100002a02082a98062a");
    ProtoReader reader(patchBytes);
    const PreloadedUserSettings decoded = PreloadedUserSettings::fromProto(reader);
    REQUIRE(decoded.status.has_value());
    REQUIRE(decoded.status->status == "dnd");
    REQUIRE_FALSE(decoded.status->customStatus.has_value());
    REQUIRE(decoded.status->showCurrentGame.has_value());
    REQUIRE(decoded.status->showCurrentGame == false);
    REQUIRE(decoded.status->statusExpiresAtMs == 1700000000000ULL);
    REQUIRE(decoded.status->unknownFields == "\x2a\x02\x08\x2a\x98\x06\x2a");
    REQUIRE_FALSE(decoded.guildFolders.has_value());
    REQUIRE_FALSE(decoded.localization.has_value());
}

TEST_CASE("Proto: Custom status patch set with custom emoji ID and fixed64 timestamps", "[proto]")
{
    StatusSettings current;
    current.status = "online";
    current.unknownFields = "\x30\x01";

    CustomStatus custom;
    custom.text = "In a meeting";
    custom.emojiId = 0x0102030405060708ULL;
    custom.expiresAtMs = 0x0203040506070809ULL;
    custom.createdAtMs = 0x030405060708090aULL;

    const std::string patch = customStatusPatch(current, custom);
    REQUIRE(toHex(patch) == "5a370a080a066f6e6c696e6512290a0c496e2061206d656574696e67110807060504030201210908070605040302290a090807060504033001");

    const std::vector<uint8_t> patchBytes = fromHex("5a370a080a066f6e6c696e6512290a0c496e2061206d656574696e67110807060504030201210908070605040302290a090807060504033001");
    ProtoReader reader(patchBytes);
    const PreloadedUserSettings decoded = PreloadedUserSettings::fromProto(reader);
    REQUIRE(decoded.status.has_value());
    REQUIRE(decoded.status->status == "online");
    REQUIRE(decoded.status->customStatus.has_value());
    REQUIRE(decoded.status->customStatus->text == "In a meeting");
    REQUIRE(decoded.status->customStatus->emojiId == 0x0102030405060708ULL);
    REQUIRE(decoded.status->customStatus->emojiName.empty());
    REQUIRE(decoded.status->customStatus->expiresAtMs == 0x0203040506070809ULL);
    REQUIRE(decoded.status->customStatus->createdAtMs == 0x030405060708090aULL);
    REQUIRE(decoded.status->unknownFields == "\x30\x01");
    REQUIRE_FALSE(decoded.guildFolders.has_value());
    REQUIRE_FALSE(decoded.localization.has_value());
}

TEST_CASE("Proto: StatusSettings unknown field handling and clean failure recovery", "[proto]")
{
    // Keep the field 5 UInt64Value wrapper and future field 99 intact.
    const std::vector<uint8_t> rawStatus = fromHex("0a050a03646e642a02082a98062a");
    const StatusSettings parsed = StatusSettings::fromProto(rawStatus);
    REQUIRE(parsed.status == "dnd");
    REQUIRE(parsed.unknownFields == "\x2a\x02\x08\x2a\x98\x06\x2a");

    // Truncated unknown field stops cleanly without throwing
    const std::vector<uint8_t> truncated = fromHex("0a050a03646e64ba010a01");
    const StatusSettings parsedTruncated = StatusSettings::fromProto(truncated);
    REQUIRE(parsedTruncated.status == "dnd");
    REQUIRE(parsedTruncated.unknownFields.empty());
}

TEST_CASE("Proto: PreloadedUserSettings mergeFrom replaces full status", "[proto]")
{
    PreloadedUserSettings local;
    StatusSettings localStatus;
    localStatus.status = "online";
    CustomStatus localCustom;
    localCustom.text = "Focusing";
    localStatus.customStatus = localCustom;
    local.status = localStatus;

    LocalizationSettings localLoc;
    localLoc.locale = "en-US";
    local.localization = localLoc;

    // Incoming partial update has status with "dnd" but custom_status is omitted (server cleared it)
    PreloadedUserSettings serverUpdate;
    StatusSettings updateStatus;
    updateStatus.status = "dnd";
    serverUpdate.status = updateStatus;

    local.mergeFrom(serverUpdate);
    REQUIRE(local.status.has_value());
    REQUIRE(local.status->status == "dnd");
    REQUIRE_FALSE(local.status->customStatus.has_value());
    REQUIRE(local.localization.has_value());
    REQUIRE(local.localization->locale == "en-US");

    // Incoming partial update without status preserves existing status
    PreloadedUserSettings locOnlyUpdate;
    LocalizationSettings newLoc;
    newLoc.locale = "fr";
    locOnlyUpdate.localization = newLoc;

    local.mergeFrom(locOnlyUpdate);
    REQUIRE(local.status.has_value());
    REQUIRE(local.status->status == "dnd");
    REQUIRE(local.localization.has_value());
    REQUIRE(local.localization->locale == "fr");
}
