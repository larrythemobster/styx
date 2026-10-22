#include <catch2/catch_test_macros.hpp>

#include "Proto/FrecencySettings.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/ProtoWriter.hpp"
#include "Platform/Base64.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace Styx::Proto;

namespace {

std::string toHex(std::span<const uint8_t> data)
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

    std::vector<uint8_t> encoded = settings.toProtoPartial();
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
