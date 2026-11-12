#include "PreloadedSettings.hpp"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ProtoReader.hpp"
#include "ProtoWriter.hpp"

namespace Styx::Proto {

namespace {

struct WireField
{
    uint32_t fieldNumber = 0;
    WireType wireType = WireType::VARINT;
    std::span<const uint8_t> rawSpan;
    std::span<const uint8_t> payloadSpan;
};

bool scanWireFields(std::span<const uint8_t> data, std::vector<WireField> &fields)
{
    ProtoReader reader(data);
    while (!reader.atEnd()) {
        const size_t fieldStart = reader.position();
        Tag tag;
        if (!reader.readTag(tag))
            return false;
        if (tag.wireType == WireType::START_GROUP || tag.wireType == WireType::END_GROUP)
            return false;

        const size_t payloadStart = reader.position();
        std::span<const uint8_t> payloadSpan;

        if (tag.wireType == WireType::LENGTH_DELIMITED) {
            if (!reader.readLengthDelimited(payloadSpan))
                return false;
        } else {
            if (!reader.skipField(tag.wireType))
                return false;
            payloadSpan = data.subspan(payloadStart, reader.position() - payloadStart);
        }

        fields.push_back(WireField{
            tag.fieldNumber,
            tag.wireType,
            data.subspan(fieldStart, reader.position() - fieldStart),
            payloadSpan
        });
    }
    return true;
}

std::string encodePackedSnowflakes(uint32_t fieldNumber, std::span<const Core::Snowflake> ids)
{
    if (ids.empty())
        return {};
    std::string packed;
    packed.reserve(ids.size() * 8);
    for (const auto &id : ids) {
        uint64_t val = static_cast<uint64_t>(id);
        for (size_t b = 0; b < 8; ++b) {
            packed.push_back(static_cast<char>(val & 0xFF));
            val >>= 8;
        }
    }
    ProtoWriter out;
    out.writeBytes(fieldNumber, packed);
    return out.takeBytes();
}

std::string encodeWrappedVarint(uint32_t fieldNumber, uint64_t value)
{
    ProtoWriter wrapper;
    wrapper.writeVarint(1, value);
    ProtoWriter out;
    out.writeBytes(fieldNumber, wrapper.bytes());
    return out.takeBytes();
}

std::string encodeRawVarint(uint32_t fieldNumber, uint64_t value)
{
    ProtoWriter out;
    out.writeVarint(fieldNumber, value);
    return out.takeBytes();
}

std::string encodeWrappedString(uint32_t fieldNumber, std::string_view value)
{
    ProtoWriter wrapper;
    wrapper.writeString(1, value);
    ProtoWriter out;
    out.writeBytes(fieldNumber, wrapper.bytes());
    return out.takeBytes();
}

} // namespace

bool PreloadedSettingsEdit::empty() const
{
    return !presenceStatus.has_value()
        && !customStatus.has_value()
        && !showCurrentGame.has_value()
        && !statusExpiresAtMs.has_value()
        && !theme.has_value()
        && !developerMode.has_value()
        && !defaultGuildsRestricted.has_value()
        && !allowAccessibilityDetection.has_value()
        && !restrictedGuildIds.has_value()
        && !friendSourceFlags.has_value()
        && !friendDiscoveryFlags.has_value()
        && !guildFolders.has_value()
        && !locale.has_value()
        && !guildPositions.has_value();
}

const std::string &PreloadedSettingsSnapshot::bytes() const
{
    return wireBytes;
}

std::optional<uint32_t> PreloadedSettingsSnapshot::dataVersion() const
{
    return version;
}

Core::Result<PreloadedSettingsSnapshot> PreloadedSettingsSnapshot::fromProto(std::string_view bytes)
{
    if (bytes.empty())
        return Core::Result<PreloadedSettingsSnapshot>::makeOk(PreloadedSettingsSnapshot{});

    const std::span<const uint8_t> rootSpan(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
    std::vector<WireField> rootFields;
    if (!scanWireFields(rootSpan, rootFields))
        return Core::Result<PreloadedSettingsSnapshot>::makeError("Malformed protobuf root wire");

    bool sawVersionsGroup = false;
    bool hasDataVersionField = false;
    uint32_t dataVersionVal = 0;

    for (const auto &rootField : rootFields) {
        const uint32_t fn = rootField.fieldNumber;
        const bool isKnownGroup = (fn == 1 || fn == 8 || fn == 11 || fn == 12 || fn == 13 || fn == 14);
        if (isKnownGroup) {
            if (rootField.wireType != WireType::LENGTH_DELIMITED)
                return Core::Result<PreloadedSettingsSnapshot>::makeError("Known group wire type must be length-delimited");

            std::vector<WireField> groupFields;
            if (!scanWireFields(rootField.payloadSpan, groupFields))
                return Core::Result<PreloadedSettingsSnapshot>::makeError("Malformed known group wire layout");

            if (fn == 1) {
                sawVersionsGroup = true;
                for (const auto &gf : groupFields) {
                    if (gf.fieldNumber == 3) {
                        if (gf.wireType != WireType::VARINT)
                            return Core::Result<PreloadedSettingsSnapshot>::makeError("Invalid versions data_version wire type");

                        ProtoReader pr(gf.payloadSpan);
                        uint64_t val = 0;
                        if (!pr.readVarint(val) || !pr.atEnd())
                            return Core::Result<PreloadedSettingsSnapshot>::makeError("Malformed versions data_version varint");
                        if (val > 0xFFFFFFFFULL)
                            return Core::Result<PreloadedSettingsSnapshot>::makeError("Versions data_version overflow");

                        dataVersionVal = static_cast<uint32_t>(val);
                        hasDataVersionField = true;
                    }
                }
            }
        }
    }

    PreloadedSettingsSnapshot snapshot;
    snapshot.wireBytes = std::string(bytes);
    if (sawVersionsGroup)
        snapshot.version = hasDataVersionField ? dataVersionVal : 0;
    else
        snapshot.version = std::nullopt;

    return Core::Result<PreloadedSettingsSnapshot>::makeOk(std::move(snapshot));
}

bool PreloadedSettingsSnapshot::hasGroup(uint32_t fieldNumber) const
{
    if (wireBytes.empty())
        return false;

    ProtoReader reader(wireBytes);
    Tag tag;
    while (reader.readTag(tag)) {
        if (tag.fieldNumber == fieldNumber && tag.wireType == WireType::LENGTH_DELIMITED)
            return true;
        reader.skipField(tag.wireType);
    }
    return false;
}

void PreloadedSettingsSnapshot::mergeFrom(const PreloadedSettingsSnapshot &partial)
{
    if (partial.wireBytes.empty())
        return;

    std::unordered_set<uint32_t> partialNumbers;
    ProtoReader partialReader(partial.wireBytes);
    Tag tag;
    while (partialReader.readTag(tag)) {
        partialNumbers.insert(tag.fieldNumber);
        partialReader.skipField(tag.wireType);
    }

    if (partialNumbers.empty())
        return;

    std::string newWireBytes;
    newWireBytes.reserve(wireBytes.size() + partial.wireBytes.size());
    if (!wireBytes.empty()) {
        ProtoReader currentReader(wireBytes);
        while (!currentReader.atEnd()) {
            const size_t fieldStart = currentReader.position();
            if (!currentReader.readTag(tag))
                break;
            currentReader.skipField(tag.wireType);
            if (!partialNumbers.count(tag.fieldNumber))
                newWireBytes.append(wireBytes.data() + fieldStart, currentReader.position() - fieldStart);
        }
    }

    newWireBytes.append(partial.wireBytes);
    wireBytes = std::move(newWireBytes);

    if (partial.hasGroup(1))
        version = partial.dataVersion();
}

Core::Result<std::string> preloadedSettingsPatch(const PreloadedSettingsSnapshot &current, const PreloadedSettingsEdit &edit)
{
    if (edit.empty())
        return Core::Result<std::string>::makeOk(std::string{});

    const bool touchPrivacy = edit.defaultGuildsRestricted.has_value()
        || edit.allowAccessibilityDetection.has_value()
        || edit.restrictedGuildIds.has_value()
        || edit.friendSourceFlags.has_value()
        || edit.friendDiscoveryFlags.has_value();

    const bool touchStatus = edit.presenceStatus.has_value()
        || edit.customStatus.has_value()
        || edit.showCurrentGame.has_value()
        || edit.statusExpiresAtMs.has_value();

    const bool touchAppearance = edit.theme.has_value()
        || edit.developerMode.has_value();
    const bool touchLocalization = edit.locale.has_value();

    const bool touchGuildFolders = edit.guildFolders.has_value()
        || edit.guildPositions.has_value();

    std::vector<WireField> currentRootFields;
    if (!current.bytes().empty()) {
        const std::span<const uint8_t> span(reinterpret_cast<const uint8_t *>(current.bytes().data()), current.bytes().size());
        if (!scanWireFields(span, currentRootFields))
            return Core::Result<std::string>::makeError("Malformed current snapshot wire");
    }

    ProtoWriter patchWriter;

    for (const auto [groupNumber, touched] : {
             std::pair{8u, touchPrivacy}, std::pair{11u, touchStatus},
             std::pair{12u, touchLocalization}, std::pair{13u, touchAppearance}, std::pair{14u, touchGuildFolders}}) {
        if (!touched)
            continue;
        std::string currentPayload;
        for (const auto &rf : currentRootFields) {
            if (rf.fieldNumber == groupNumber && rf.wireType == WireType::LENGTH_DELIMITED)
                currentPayload.append(reinterpret_cast<const char *>(rf.payloadSpan.data()), rf.payloadSpan.size());
        }

        std::map<uint32_t, std::string> replacements;

        if (groupNumber == 8) {
            if (edit.restrictedGuildIds.has_value())
                replacements[3] = encodePackedSnowflakes(3, *edit.restrictedGuildIds);
            if (edit.defaultGuildsRestricted.has_value())
                replacements[4] = encodeRawVarint(4, *edit.defaultGuildsRestricted ? 1 : 0);
            if (edit.allowAccessibilityDetection.has_value())
                replacements[5] = encodeRawVarint(5, *edit.allowAccessibilityDetection ? 1 : 0);
            if (edit.friendSourceFlags.has_value())
                replacements[9] = encodeWrappedVarint(9, *edit.friendSourceFlags);
            if (edit.friendDiscoveryFlags.has_value())
                replacements[10] = encodeWrappedVarint(10, *edit.friendDiscoveryFlags);
        } else if (groupNumber == 11) {
            if (edit.presenceStatus.has_value())
                replacements[1] = encodeWrappedString(1, *edit.presenceStatus);
            if (edit.customStatus.has_value()) {
                if ((*edit.customStatus).has_value()) {
                    ProtoWriter out;
                    out.writeBytes(2, (*edit.customStatus)->toProto());
                    replacements[2] = out.takeBytes();
                } else {
                    replacements[2] = "";
                }
            }
            if (edit.showCurrentGame.has_value())
                replacements[3] = encodeWrappedVarint(3, *edit.showCurrentGame ? 1 : 0);
            if (edit.statusExpiresAtMs.has_value()) {
                ProtoWriter out;
                out.writeFixed64(4, *edit.statusExpiresAtMs);
                replacements[4] = out.takeBytes();
            }
        } else if (groupNumber == 12) {
            if (edit.locale.has_value())
                replacements[1] = encodeWrappedString(1, *edit.locale);
        } else if (groupNumber == 13) {
            if (edit.theme.has_value())
                replacements[1] = encodeRawVarint(1, *edit.theme);
            if (edit.developerMode.has_value())
                replacements[2] = encodeRawVarint(2, *edit.developerMode ? 1 : 0);
        } else if (groupNumber == 14) {
            if (edit.guildFolders.has_value()) {
                if (!edit.guildFolders->empty()) {
                    ProtoWriter out;
                    for (const auto &folder : *edit.guildFolders) {
                        std::string folderMsg;
                        if (!folder.guildIds.empty())
                            folderMsg.append(encodePackedSnowflakes(1, folder.guildIds));
                        if (folder.id.has_value())
                            folderMsg.append(encodeWrappedVarint(2, static_cast<uint64_t>(*folder.id)));
                        if (folder.name.has_value())
                            folderMsg.append(encodeWrappedString(3, *folder.name));
                        if (folder.color.has_value())
                            folderMsg.append(encodeWrappedVarint(4, *folder.color));
                        out.writeBytes(1, folderMsg);
                    }
                    replacements[1] = out.takeBytes();
                } else {
                    replacements[1] = "";
                }
            }
            if (edit.guildPositions.has_value())
                replacements[2] = encodePackedSnowflakes(2, *edit.guildPositions);
        }

        std::string newGroupPayload;
        std::unordered_set<uint32_t> emitted;

        if (!currentPayload.empty()) {
            std::vector<WireField> fields;
            const std::span<const uint8_t> span(reinterpret_cast<const uint8_t *>(currentPayload.data()), currentPayload.size());
            scanWireFields(span, fields);
            for (const auto &f : fields) {
                auto it = replacements.find(f.fieldNumber);
                if (it != replacements.end()) {
                    if (!emitted.count(f.fieldNumber)) {
                        emitted.insert(f.fieldNumber);
                        if (!it->second.empty())
                            newGroupPayload.append(it->second);
                    }
                } else {
                    newGroupPayload.append(reinterpret_cast<const char *>(f.rawSpan.data()), f.rawSpan.size());
                }
            }
        }

        for (const auto &[fieldNum, replacement] : replacements) {
            if (!emitted.count(fieldNum)) {
                emitted.insert(fieldNum);
                if (!replacement.empty())
                    newGroupPayload.append(replacement);
            }
        }

        patchWriter.writeBytes(groupNumber, newGroupPayload);
    }

    return Core::Result<std::string>::makeOk(patchWriter.takeBytes());
}

} // namespace Styx::Proto
