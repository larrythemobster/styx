#include "UserSettings.hpp"

#include "ProtoReader.hpp"
#include "ProtoWriter.hpp"

namespace Styx {
namespace Proto {

GuildFolder GuildFolder::fromProto(ProtoReader &reader)
{
    GuildFolder folder;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // repeated fixed64 guild_ids
        case 1: {
            if (tag.wireType == WireType::FIXED64) {
                uint64_t id;
                if (reader.readFixed64(id))
                    folder.guildIds.push_back(Core::Snowflake(id));
            } else if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> packed;
                if (reader.readLengthDelimited(packed)) {
                    ProtoReader packedReader(packed);
                    uint64_t id;
                    while (!packedReader.atEnd() && packedReader.readFixed64(id))
                        folder.guildIds.push_back(Core::Snowflake(id));
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional google.protobuf.Int64Value id
        case 2: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    folder.id = readInt64Value(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional google.protobuf.StringValue name
        case 3: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    folder.name = readStringValue(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional google.protobuf.UInt64Value color
        case 4: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    folder.color = readUInt64Value(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return folder;
}

GuildFolders GuildFolders::fromProto(ProtoReader &reader)
{
    GuildFolders guildFolders;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // repeated GuildFolder folders
        case 1: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    guildFolders.folders.push_back(GuildFolder::fromProto(nestedReader));
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // repeated fixed64 guild_positions
        case 2: {
            if (tag.wireType == WireType::FIXED64) {
                uint64_t id;
                if (reader.readFixed64(id))
                    guildFolders.guildPositions.push_back(Core::Snowflake(id));
            } else if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> packed;
                if (reader.readLengthDelimited(packed)) {
                    ProtoReader packedReader(packed);
                    uint64_t id;
                    while (!packedReader.atEnd() && packedReader.readFixed64(id))
                        guildFolders.guildPositions.push_back(Core::Snowflake(id));
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return guildFolders;
}

CustomStatus CustomStatus::fromProto(ProtoReader &reader)
{
    CustomStatus custom;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // string text
        case 1: {
            if (tag.wireType == WireType::LENGTH_DELIMITED)
                custom.text = readString(reader);
            else
                reader.skipField(tag.wireType);
            break;
        }
        // fixed64 emoji_id
        case 2: {
            uint64_t id;
            if (tag.wireType == WireType::FIXED64 && reader.readFixed64(id))
                custom.emojiId = id;
            else
                reader.skipField(tag.wireType);
            break;
        }
        // string emoji_name
        case 3: {
            if (tag.wireType == WireType::LENGTH_DELIMITED)
                custom.emojiName = readString(reader);
            else
                reader.skipField(tag.wireType);
            break;
        }
        // fixed64 expires_at_ms
        case 4: {
            uint64_t value;
            if (tag.wireType == WireType::FIXED64 && reader.readFixed64(value))
                custom.expiresAtMs = value;
            else
                reader.skipField(tag.wireType);
            break;
        }
        // fixed64 created_at_ms
        case 5: {
            uint64_t value;
            if (tag.wireType == WireType::FIXED64 && reader.readFixed64(value))
                custom.createdAtMs = value;
            else
                reader.skipField(tag.wireType);
            break;
        }
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return custom;
}

std::string CustomStatus::toProto() const
{
    ProtoWriter writer;
    if (!text.empty())
        writer.writeString(1, text);
    if (emojiId != 0)
        writer.writeFixed64(2, emojiId);
    if (!emojiName.empty())
        writer.writeString(3, emojiName);
    if (expiresAtMs != 0)
        writer.writeFixed64(4, expiresAtMs);
    if (createdAtMs != 0)
        writer.writeFixed64(5, createdAtMs);
    return writer.takeBytes();
}

StatusSettings StatusSettings::fromProto(std::span<const uint8_t> bytes)
{
    StatusSettings settings;
    ProtoReader reader(bytes);
    Tag tag;

    for (;;) {
        const size_t fieldStart = reader.position();
        if (!reader.readTag(tag))
            break;

        if (tag.fieldNumber == 1 && tag.wireType == WireType::LENGTH_DELIMITED) {
            std::span<const uint8_t> nested;
            if (!reader.readLengthDelimited(nested))
                break;
            ProtoReader nestedReader(nested);
            settings.status = readStringValue(nestedReader);
        } else if (tag.fieldNumber == 2 && tag.wireType == WireType::LENGTH_DELIMITED) {
            std::span<const uint8_t> nested;
            if (!reader.readLengthDelimited(nested))
                break;
            ProtoReader nestedReader(nested);
            settings.customStatus = CustomStatus::fromProto(nestedReader);
        } else if (tag.fieldNumber == 3 && tag.wireType == WireType::LENGTH_DELIMITED) {
            std::span<const uint8_t> nested;
            if (!reader.readLengthDelimited(nested))
                break;
            ProtoReader nestedReader(nested);
            settings.showCurrentGame = readBoolValue(nestedReader);
        } else if (tag.fieldNumber == 4 && tag.wireType == WireType::FIXED64) {
            uint64_t value = 0;
            if (!reader.readFixed64(value))
                break;
            settings.statusExpiresAtMs = value;
        } else {
            if (!reader.skipField(tag.wireType))
                break;
            const size_t fieldLen = reader.position() - fieldStart;
            settings.unknownFields.append(reinterpret_cast<const char *>(bytes.data() + fieldStart), fieldLen);
        }
    }

    return settings;
}

LocalizationSettings LocalizationSettings::fromProto(ProtoReader &reader)
{
    LocalizationSettings settings;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // optional google.protobuf.StringValue locale
        case 1: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    settings.locale = readStringValue(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return settings;
}

PreloadedUserSettings PreloadedUserSettings::fromProto(ProtoReader &reader)
{
    PreloadedUserSettings settings;
    Tag tag;

    while (reader.readTag(tag)) {
        switch (tag.fieldNumber) {
        // optional StatusSettings status
        case 11: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> nested;
                if (reader.readLengthDelimited(nested))
                    settings.status = StatusSettings::fromProto(nested);
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional LocalizationSettings localization
        case 12: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    settings.localization = LocalizationSettings::fromProto(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        // optional GuildFolders guild_folders
        case 14: {
            if (tag.wireType == WireType::LENGTH_DELIMITED) {
                std::span<const uint8_t> nested;
                if (reader.readLengthDelimited(nested)) {
                    ProtoReader nestedReader(nested);
                    settings.guildFolders = GuildFolders::fromProto(nestedReader);
                }
            } else {
                reader.skipField(tag.wireType);
            }
            break;
        }
        default:
            reader.skipField(tag.wireType);
            break;
        }
    }

    return settings;
}

} // namespace Proto
} // namespace Styx
