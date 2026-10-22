#include "FrecencySettings.hpp"

#include "ProtoReader.hpp"
#include "ProtoWriter.hpp"

namespace Styx {
namespace Proto {

namespace {

std::optional<uint64_t> readVarintField(ProtoReader &reader, WireType wireType)
{
    if (wireType != WireType::VARINT) {
        reader.skipField(wireType);
        return std::nullopt;
    }

    uint64_t value;
    if (!reader.readVarint(value))
        return std::nullopt;
    return value;
}

int32_t toInt32(uint64_t value)
{
    return static_cast<int32_t>(static_cast<uint32_t>(value));
}

/* 
FrecencyItem
{
    uint32 total_uses = 1;
    repeated uint64 recent_uses = 2;
    int32 frecency = 3;
    int32 score = 4;
}
*/
std::vector<uint8_t> writeFrecencyItem(const FrecencyItem &item)
{
    ProtoWriter writer;
    if (item.totalUses)
        writer.writeVarint(1, item.totalUses);
    writer.writePackedVarints(2, item.recentUses);
    if (item.frecency)
        writer.writeInt32(3, item.frecency);
    if (item.score)
        writer.writeInt32(4, item.score);
    return writer.takeBytes();
}

/*
EmojiFrecency
{
    map<string, FrecencyItem> emojis = 1;
}
*/
std::vector<uint8_t> writeEmojiFrecency(const EmojiFrecency &frecency)
{
    ProtoWriter writer;
    for (const auto &[key, item] : frecency.emojis) {
        ProtoWriter entry;
        entry.writeString(1, key);
        entry.writeBytes(2, writeFrecencyItem(item));
        writer.writeBytes(1, entry.bytes());
    }
    return writer.takeBytes();
}

void readEmojiEntry(ProtoReader &reader, EmojiFrecency &frecency)
{
    std::string key;
    FrecencyItem item;
    Tag tag;

    while (reader.readTag(tag)) {
        if (tag.wireType != WireType::LENGTH_DELIMITED) {
            reader.skipField(tag.wireType);
            continue;
        }

        if (tag.fieldNumber == 1) {
            key = readString(reader);
            continue;
        }

        std::span<const uint8_t> nested;
        if (!reader.readLengthDelimited(nested))
            break;
        if (tag.fieldNumber == 2) {
            ProtoReader nestedReader(nested);
            item = FrecencyItem::fromProto(nestedReader);
        }
    }

    if (!key.empty())
        frecency.emojis.insert_or_assign(std::move(key), std::move(item));
}

std::vector<uint8_t> writeFavoriteGif(const FavoriteGif &gif)
{
    ProtoWriter writer;
    if (gif.format != GifType::None)
        writer.writeVarint(1, static_cast<uint64_t>(gif.format));
    if (!gif.src.empty())
        writer.writeString(2, gif.src);
    if (gif.width)
        writer.writeVarint(3, gif.width);
    if (gif.height)
        writer.writeVarint(4, gif.height);
    if (gif.order)
        writer.writeVarint(5, gif.order);
    return writer.takeBytes();
}

void readFavoriteGif(std::span<const uint8_t> bytes, FavoriteGif &gif)
{
    gif.messageAsReceived.assign(bytes.begin(), bytes.end());

    ProtoReader reader(bytes);
    Tag tag;
    while (reader.readTag(tag)) {
        if (tag.fieldNumber == 2 && tag.wireType == WireType::LENGTH_DELIMITED) {
            gif.src = readString(reader);
            continue;
        }

        const std::optional<uint64_t> value = readVarintField(reader, tag.wireType);
        if (!value)
            continue;

        switch (tag.fieldNumber) {
        case 1:
            gif.format = static_cast<GifType>(*value);
            break;
        case 3:
            gif.width = static_cast<uint32_t>(*value);
            break;
        case 4:
            gif.height = static_cast<uint32_t>(*value);
            break;
        case 5:
            gif.order = static_cast<uint32_t>(*value);
            break;
        default:
            break;
        }
    }
}

std::optional<FavoriteGif> readFavoriteGifEntry(std::span<const uint8_t> bytes)
{
    FavoriteGif gif;
    ProtoReader reader(bytes);
    Tag tag;

    while (reader.readTag(tag)) {
        if (tag.wireType != WireType::LENGTH_DELIMITED) {
            reader.skipField(tag.wireType);
            continue;
        }

        if (tag.fieldNumber == 1) {
            gif.url = readString(reader);
            continue;
        }

        std::span<const uint8_t> nested;
        if (!reader.readLengthDelimited(nested))
            break;
        if (tag.fieldNumber == 2)
            readFavoriteGif(nested, gif);
    }

    return gif.url.empty() ? std::nullopt : std::optional(gif);
}

/*
Versions
{
    uint32 client_version = 1;
    uint32 server_version = 2;
    uint32 data_version = 3;
}
*/
std::optional<uint32_t> readDataVersion(ProtoReader &reader)
{
    std::optional<uint32_t> dataVersion;
    Tag tag;

    while (reader.readTag(tag)) {
        const std::optional<uint64_t> value = readVarintField(reader, tag.wireType);
        if (value && tag.fieldNumber == 3)
            dataVersion = static_cast<uint32_t>(*value);
    }

    return dataVersion;
}

} // namespace

FrecencyItem FrecencyItem::fromProto(ProtoReader &reader)
{
    FrecencyItem item;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // uint32 total_uses = 1
        case 1:
            if (const std::optional<uint64_t> value = readVarintField(reader, tag.wireType))
                item.totalUses = static_cast<uint32_t>(*value);
            break;
        // repeated uint64 recent_uses = 2 [packed], but unpacked is valid proto too
        case 2:
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> packed;
                if (reader.readLengthDelimited(packed)) {
                    ProtoReader packedReader(packed);
                    uint64_t value;
                    while (!packedReader.atEnd() && packedReader.readVarint(value))
                        item.recentUses.push_back(value);
                }
            } else if (const std::optional<uint64_t> value =
                               readVarintField(reader, tag.wireType)) {
                item.recentUses.push_back(*value);
            }
            break;
        // int32 frecency = 3
        case 3:
            if (const std::optional<uint64_t> value = readVarintField(reader, tag.wireType))
                item.frecency = toInt32(*value);
            break;
        // int32 score = 4
        case 4:
            if (const std::optional<uint64_t> value = readVarintField(reader, tag.wireType))
                item.score = toInt32(*value);
            break;
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return item;
}

EmojiFrecency EmojiFrecency::fromProto(ProtoReader &reader)
{
    EmojiFrecency frecency;
    Tag tag;

    while (reader.readTag(tag)) {
        // map<string, FrecencyItem> emojis = 1
        if (tag.fieldNumber == 1 && tag.wireType == WireType::LENGTH_DELIMITED) {
            std::span<const uint8_t> nested;
            if (reader.readLengthDelimited(nested)) {
                ProtoReader nestedReader(nested);
                readEmojiEntry(nestedReader, frecency);
            }
        } else {
            reader.skipField(tag.wireType);
        }
    }

    return frecency;
}

FavoriteGifs FavoriteGifs::fromProto(std::span<const uint8_t> bytes)
{
    FavoriteGifs favorites;
    ProtoReader reader(bytes);
    Tag tag;

    for (;;) {
        const size_t fieldStart = reader.position();
        if (!reader.readTag(tag))
            break;

        if (tag.fieldNumber == 1 && tag.wireType == WireType::LENGTH_DELIMITED) {
            std::span<const uint8_t> entry;
            if (!reader.readLengthDelimited(entry))
                break;
            if (const std::optional<FavoriteGif> gif = readFavoriteGifEntry(entry))
                favorites.gifs.push_back(*gif);
        } else if (tag.fieldNumber == 2 && tag.wireType == WireType::VARINT) {
            uint64_t value = 0;
            if (!reader.readVarint(value))
                break;
            favorites.hideTooltip = value != 0;
        } else {
            if (!reader.skipField(tag.wireType))
                break;
            const size_t fieldLen = reader.position() - fieldStart;
            favorites.unknownFields.insert(favorites.unknownFields.end(),
                                           bytes.data() + fieldStart,
                                           bytes.data() + fieldStart + fieldLen);
        }
    }

    return favorites;
}

std::vector<uint8_t> FavoriteGifs::toProto() const
{
    ProtoWriter writer;
    for (const FavoriteGif &gif : gifs) {
        ProtoWriter entry;
        entry.writeString(1, gif.url);
        entry.writeBytes(2, gif.messageAsReceived.empty() ? writeFavoriteGif(gif) : gif.messageAsReceived);
        writer.writeBytes(1, entry.bytes());
    }
    if (hideTooltip)
        writer.writeVarint(2, 1);
    std::vector<uint8_t> res = writer.takeBytes();
    res.insert(res.end(), unknownFields.begin(), unknownFields.end());
    return res;
}

FrecencyUserSettings FrecencyUserSettings::fromProto(ProtoReader &reader)
{
    FrecencyUserSettings settings;
    Tag tag;

    while (reader.readTag(tag)) {
        if (tag.wireType != WireType::LENGTH_DELIMITED) {
            reader.skipField(tag.wireType);
            continue;
        }

        std::span<const uint8_t> nested;
        if (!reader.readLengthDelimited(nested))
            break;

        ProtoReader nestedReader(nested);
        switch (tag.fieldNumber) {
        // Versions versions
        case 1:
            settings.dataVersion = readDataVersion(nestedReader);
            break;
        case 2:
            settings.favoriteGifs = FavoriteGifs::fromProto(nested);
            break;
        // EmojiFrecency emoji_frecency
        case 6:
            settings.emojiFrecency = EmojiFrecency::fromProto(nestedReader);
            break;
        // EmojiFrecency emoji_reaction_frecency
        case 13:
            settings.emojiReactionFrecency = EmojiFrecency::fromProto(nestedReader);
            break;
        default:
            break;
        }
    }

    return settings;
}

std::vector<uint8_t> FrecencyUserSettings::toProtoPartial() const
{
    ProtoWriter writer;
    if (favoriteGifs)
        writer.writeBytes(2, favoriteGifs->toProto());
    if (emojiFrecency)
        writer.writeBytes(6, writeEmojiFrecency(*emojiFrecency));
    if (emojiReactionFrecency)
        writer.writeBytes(13, writeEmojiFrecency(*emojiReactionFrecency));
    return writer.takeBytes();
}

} // namespace Proto
} // namespace Styx
