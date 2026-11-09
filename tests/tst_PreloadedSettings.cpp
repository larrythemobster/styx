#include <catch2/catch_test_macros.hpp>

#include "Proto/PreloadedSettings.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/ProtoWriter.hpp"
#include "Proto/UserSettings.hpp"

#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

using namespace Styx;
using namespace Styx::Proto;

namespace {

template<class Bytes> std::string toHex(const Bytes &data)
{
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char byte : data) {
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
        auto byte = static_cast<uint8_t>(std::stoul(byteString, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

PreloadedSettingsSnapshot snapshotFromHex(std::string_view hex)
{
    const auto raw = fromHex(hex);
    auto res = PreloadedSettingsSnapshot::fromProto(
        std::string_view(reinterpret_cast<const char *>(raw.data()), raw.size())
    );
    REQUIRE(res.success());
    return std::move(*res.value);
}

} // namespace

TEST_CASE("PreloadedSettings: Literal byte fixture with correct wrappers and edited groups only", "[proto]")
{
    PreloadedSettingsSnapshot current;
    PreloadedSettingsEdit edit;
    edit.defaultGuildsRestricted = true;
    edit.friendSourceFlags = 15;
    edit.theme = 1;
    edit.developerMode = true;

    const auto patch = preloadedSettingsPatch(current, edit);
    REQUIRE(patch.success());
    REQUIRE(toHex(*patch.value) == "420620014a02080f6a0408011001");

    const auto decoded = PreloadedSettingsSnapshot::fromProto(*patch.value);
    REQUIRE(decoded.success());
    REQUIRE(decoded.value->hasGroup(8));
    REQUIRE(decoded.value->hasGroup(13));
    REQUIRE_FALSE(decoded.value->hasGroup(11));
    REQUIRE_FALSE(decoded.value->hasGroup(14));
}

TEST_CASE("PreloadedSettings: Unknown nested siblings preserved across all four groups", "[proto]")
{
    SECTION("Status preserves unknown field 99 varint") {
        const auto current = snapshotFromHex("5a0d0a080a066f6e6c696e6598062a");
        PreloadedSettingsEdit edit;
        edit.presenceStatus = "dnd";

        const auto patch = preloadedSettingsPatch(current, edit);
        REQUIRE(patch.success());
        REQUIRE(toHex(*patch.value) == "5a0a0a050a03646e6498062a");

        ProtoReader reader(*patch.value);
        const auto parsed = PreloadedUserSettings::fromProto(reader);
        REQUIRE(parsed.status.has_value());
        REQUIRE(parsed.status->status == "dnd");
        REQUIRE(parsed.status->unknownFields == "\x98\x06\x2a");
    }

    SECTION("Appearance preserves unknown field 5 fixed32") {
        const auto current = snapshotFromHex("6a0708012d78563412");
        PreloadedSettingsEdit edit;
        edit.developerMode = true;

        const auto patch = preloadedSettingsPatch(current, edit);
        REQUIRE(patch.success());
        REQUIRE(toHex(*patch.value) == "6a0908012d785634121001");
    }

    SECTION("Privacy preserves unknown field 99 varint") {
        const auto current = snapshotFromHex("42052001980607");
        PreloadedSettingsEdit edit;
        edit.allowAccessibilityDetection = true;

        const auto patch = preloadedSettingsPatch(current, edit);
        REQUIRE(patch.success());
        REQUIRE(toHex(*patch.value) == "420720019806072801");
    }

    SECTION("Guild folders preserves existing positions and unknown field 99") {
        const auto current = snapshotFromHex("720d12086400000000000000980601");
        PreloadedSettingsEdit edit;
        GuildFolder folder;
        folder.name = "Styx";
        edit.guildFolders = std::vector<GuildFolder>{ folder };

        const auto patch = preloadedSettingsPatch(current, edit);
        REQUIRE(patch.success());


        ProtoReader rootReader(*patch.value);
        const auto parsed = PreloadedUserSettings::fromProto(rootReader);
        REQUIRE(parsed.guildFolders.has_value());
        REQUIRE(parsed.guildFolders->folders.size() == 1);
        REQUIRE(parsed.guildFolders->folders[0].name == "Styx");
        REQUIRE(parsed.guildFolders->guildPositions.size() == 1);
        REQUIRE(static_cast<uint64_t>(parsed.guildFolders->guildPositions[0]) == 100u);

        ProtoReader folderGroupReader(*patch.value);
        Tag tag;
        std::span<const uint8_t> folderGroupPayload;
        while (folderGroupReader.readTag(tag)) {
            if (tag.fieldNumber == 14 && tag.wireType == WireType::LENGTH_DELIMITED) {
                REQUIRE(folderGroupReader.readLengthDelimited(folderGroupPayload));
                break;
            }
            folderGroupReader.skipField(tag.wireType);
        }

        ProtoReader innerReader(folderGroupPayload);
        bool foundField99 = false;
        while (innerReader.readTag(tag)) {
            if (tag.fieldNumber == 99) {
                REQUIRE(tag.wireType == WireType::VARINT);
                uint64_t val = 0;
                REQUIRE(innerReader.readVarint(val));
                REQUIRE(val == 1u);
                foundField99 = true;
            } else {
                innerReader.skipField(tag.wireType);
            }
        }
        REQUIRE(foundField99);
    }
}

TEST_CASE("PreloadedSettings: Clearing custom status versus leaving it untouched", "[proto]")
{
    const auto current = snapshotFromHex(
        "5a1c0a080a066f6e6c696e65120c0a04427573791a04f09f94a51a020801"
    );

    SECTION("Clearing custom status removes field 2 while preserving presence and game") {
        PreloadedSettingsEdit edit;
        edit.customStatus = std::optional<CustomStatus>(std::nullopt);

        const auto patch = preloadedSettingsPatch(current, edit);
        REQUIRE(patch.success());

        ProtoReader reader(*patch.value);
        const auto parsed = PreloadedUserSettings::fromProto(reader);
        REQUIRE(parsed.status.has_value());
        REQUIRE(parsed.status->status == "online");
        REQUIRE_FALSE(parsed.status->customStatus.has_value());
        REQUIRE(parsed.status->showCurrentGame.has_value());
        REQUIRE(parsed.status->showCurrentGame == true);
    }

    SECTION("Leaving custom status unset preserves existing custom status") {
        PreloadedSettingsEdit edit;
        edit.showCurrentGame = false;

        const auto patch = preloadedSettingsPatch(current, edit);
        REQUIRE(patch.success());

        ProtoReader reader(*patch.value);
        const auto parsed = PreloadedUserSettings::fromProto(reader);
        REQUIRE(parsed.status.has_value());
        REQUIRE(parsed.status->status == "online");
        REQUIRE(parsed.status->customStatus.has_value());
        REQUIRE(parsed.status->customStatus->text == "Busy");
        REQUIRE(parsed.status->customStatus->emojiName == "\xf0\x9f\x94\xa5");
        REQUIRE(parsed.status->showCurrentGame.has_value());
        REQUIRE(parsed.status->showCurrentGame == false);
    }
}

TEST_CASE("PreloadedSettings: False and 0 updates serialize explicitly without omission", "[proto]")
{
    PreloadedSettingsSnapshot current;
    PreloadedSettingsEdit edit;
    edit.showCurrentGame = false;
    edit.defaultGuildsRestricted = false;
    edit.allowAccessibilityDetection = false;
    edit.theme = 0;
    edit.developerMode = false;
    edit.friendSourceFlags = 0;
    edit.friendDiscoveryFlags = 0;
    edit.statusExpiresAtMs = 0;

    const auto patch = preloadedSettingsPatch(current, edit);
    REQUIRE(patch.success());

    REQUIRE(toHex(*patch.value) == "420c200028004a020800520208005a0d1a0208002100000000000000006a0408001000");

}

TEST_CASE("PreloadedSettings: Privacy repeated IDs with >32-bit snowflakes on packed and unpacked baselines", "[proto]")
{
    SECTION("Unpacked baseline replaced by packed list") {
        const auto current = snapshotFromHex(
            "4212190807060504030201190102030405060708"
        );
        PreloadedSettingsEdit edit;
        edit.restrictedGuildIds = std::vector<Core::Snowflake>{
            Core::Snowflake(0xAABBCCDDEEFF0011ULL)
        };

        const auto patch = preloadedSettingsPatch(current, edit);
        REQUIRE(patch.success());

        const std::string hex = toHex(*patch.value);
        REQUIRE(hex == "420a1a081100ffeeddccbbaa");
    }

    SECTION("Packed baseline cleared by empty vector") {
        const auto current = snapshotFromHex(
            "421420011a1001020304050607080807060504030201"
        );
        PreloadedSettingsEdit edit;
        edit.restrictedGuildIds = std::vector<Core::Snowflake>{};

        const auto patch = preloadedSettingsPatch(current, edit);
        REQUIRE(patch.success());
        REQUIRE(toHex(*patch.value) == "42022001");
    }
}

TEST_CASE("PreloadedSettings: Guild list clearing independent of positions and high snowflakes", "[proto]")
{
    GuildFolder folder;
    folder.guildIds = { Core::Snowflake(0x1122334455667788ULL) };
    folder.id = -42LL;
    folder.name = "General";
    folder.color = 0xFF5733ULL;

    PreloadedSettingsSnapshot current;
    PreloadedSettingsEdit initial;
    initial.guildFolders = std::vector<GuildFolder>{ folder };
    initial.guildPositions = std::vector<Core::Snowflake>{
        Core::Snowflake(0x9988776655443322ULL)
    };
    const auto setupPatch = preloadedSettingsPatch(current, initial);
    REQUIRE(setupPatch.success());

    const auto snapshot = PreloadedSettingsSnapshot::fromProto(*setupPatch.value);
    REQUIRE(snapshot.success());

    SECTION("Clearing folders leaves positions intact") {
        PreloadedSettingsEdit edit;
        edit.guildFolders = std::vector<GuildFolder>{};

        const auto patch = preloadedSettingsPatch(*snapshot.value, edit);
        REQUIRE(patch.success());

        ProtoReader reader(*patch.value);
        const auto parsed = PreloadedUserSettings::fromProto(reader);
        REQUIRE(parsed.guildFolders.has_value());
        REQUIRE(parsed.guildFolders->folders.empty());
        REQUIRE(parsed.guildFolders->guildPositions.size() == 1);
        REQUIRE(static_cast<uint64_t>(parsed.guildFolders->guildPositions[0]) == 0x9988776655443322ULL);
    }

    SECTION("Clearing positions leaves folders and negative id intact") {
        PreloadedSettingsEdit edit;
        edit.guildPositions = std::vector<Core::Snowflake>{};

        const auto patch = preloadedSettingsPatch(*snapshot.value, edit);
        REQUIRE(patch.success());

        ProtoReader reader(*patch.value);
        const auto parsed = PreloadedUserSettings::fromProto(reader);
        REQUIRE(parsed.guildFolders.has_value());
        REQUIRE(parsed.guildFolders->guildPositions.empty());
        REQUIRE(parsed.guildFolders->folders.size() == 1);
        const auto &f = parsed.guildFolders->folders[0];
        REQUIRE(f.name == "General");
        REQUIRE(f.id.has_value());
        REQUIRE(*f.id == -42LL);
        REQUIRE(f.color.has_value());
        REQUIRE(*f.color == 0xFF5733ULL);
        REQUIRE(f.guildIds.size() == 1);
        REQUIRE(static_cast<uint64_t>(f.guildIds[0]) == 0x1122334455667788ULL);
    }
}

TEST_CASE("PreloadedSettings: Partial server root merge replaces present empty group and preserves omitted others", "[proto]")
{
    const auto local = snapshotFromHex("0a02180a420220015a070a050a03646e64");
    REQUIRE(local.dataVersion() == 10u);
    REQUIRE(local.hasGroup(8));
    REQUIRE(local.hasGroup(11));

    const auto partial = snapshotFromHex("0a02180b5a00");
    REQUIRE(partial.dataVersion() == 11u);
    REQUIRE(partial.hasGroup(11));
    REQUIRE_FALSE(partial.hasGroup(8));

    auto merged = local;
    merged.mergeFrom(partial);

    REQUIRE(merged.dataVersion() == 11u);
    REQUIRE(merged.hasGroup(8));
    REQUIRE(merged.hasGroup(11));

    ProtoReader reader(merged.bytes());
    const auto parsed = PreloadedUserSettings::fromProto(reader);
    REQUIRE(parsed.status.has_value());
    REQUIRE_FALSE(parsed.status->status.has_value());
    REQUIRE_FALSE(parsed.status->customStatus.has_value());
}

TEST_CASE("PreloadedSettings: Version parsing and overflow handling", "[proto]")
{
    SECTION("Valid data version") {
        const auto snap = snapshotFromHex("0a02182a");
        REQUIRE(snap.dataVersion() == 42u);
    }

    SECTION("Present versions group with absent field 3 yields version 0") {
        const auto snap = snapshotFromHex("0a00");
        REQUIRE(snap.dataVersion() == 0u);
    }

    SECTION("Omitted versions group yields nullopt") {
        const auto snap = snapshotFromHex("5a00");
        REQUIRE_FALSE(snap.dataVersion().has_value());
    }

    SECTION("Data version exceeding 32-bit unsigned int fails validation") {
        const auto raw = fromHex("0a06188080808010");
        const auto res = PreloadedSettingsSnapshot::fromProto(
            std::string_view(reinterpret_cast<const char *>(raw.data()), raw.size())
        );
        REQUIRE_FALSE(res.success());
    }

    SECTION("Non-varint wire type for field 3 fails validation") {
        const auto raw = fromHex("0a09190100000000000000");
        const auto res = PreloadedSettingsSnapshot::fromProto(
            std::string_view(reinterpret_cast<const char *>(raw.data()), raw.size())
        );
        REQUIRE_FALSE(res.success());
    }
}

TEST_CASE("PreloadedSettings: Duplicate root fragments combine before editing", "[proto]")
{
    const auto current = snapshotFromHex("5a070a050a03646e645a0398062a");
    PreloadedSettingsEdit edit;
    edit.showCurrentGame = true;

    const auto patch = preloadedSettingsPatch(current, edit);
    REQUIRE(patch.success());

    ProtoReader rootReader(*patch.value);
    size_t group11Count = 0;
    Tag tag;
    std::span<const uint8_t> statusPayload;
    while (rootReader.readTag(tag)) {
        if (tag.fieldNumber == 11) {
            group11Count++;
            REQUIRE(tag.wireType == WireType::LENGTH_DELIMITED);
            REQUIRE(rootReader.readLengthDelimited(statusPayload));
        } else {
            rootReader.skipField(tag.wireType);
        }
    }
    REQUIRE(group11Count == 1);

    const auto parsedStatus = StatusSettings::fromProto(statusPayload);
    REQUIRE(parsedStatus.status.has_value());
    REQUIRE(parsedStatus.status == "dnd");
    REQUIRE(parsedStatus.showCurrentGame.has_value());
    REQUIRE(parsedStatus.showCurrentGame == true);
    REQUIRE(parsedStatus.unknownFields == "\x98\x06\x2a");
}

TEST_CASE("PreloadedSettings: Duplicate occurrences of edited field are all removed and replacement emitted once", "[proto]")
{
    const auto current = snapshotFromHex("5a160a070a0566697273740a080a067365636f6e6498062a");
    PreloadedSettingsEdit edit;
    edit.presenceStatus = "third";

    const auto patch = preloadedSettingsPatch(current, edit);
    REQUIRE(patch.success());

    REQUIRE(toHex(*patch.value) == "5a0c0a070a05746869726498062a");

    ProtoReader reader(*patch.value);
    const auto parsed = PreloadedUserSettings::fromProto(reader);
    REQUIRE(parsed.status.has_value());
    REQUIRE(parsed.status->status == "third");
    REQUIRE(parsed.status->unknownFields == "\x98\x06\x2a");
}

TEST_CASE("PreloadedSettings: Unknown wrapper payload preserved in untouched sibling", "[proto]")
{
    const auto current = snapshotFromHex("5a0d0a0b0a066f6e6c696e6598062a");
    PreloadedSettingsEdit edit;
    edit.showCurrentGame = true;

    const auto patch = preloadedSettingsPatch(current, edit);
    REQUIRE(patch.success());

    const std::string hex = toHex(*patch.value);
    REQUIRE(hex == "5a110a0b0a066f6e6c696e6598062a1a020801");
}

TEST_CASE("PreloadedSettings: Malformed wire rejected before writing", "[proto]")
{
    SECTION("Truncated tag varint") {
        const uint8_t bytes[] = { 0x80 };
        const auto res = PreloadedSettingsSnapshot::fromProto(
            std::string_view(reinterpret_cast<const char *>(bytes), sizeof(bytes))
        );
        REQUIRE_FALSE(res.success());
    }

    SECTION("Field 0 is rejected") {
        const uint8_t bytes[] = { 0x00 };
        const auto res = PreloadedSettingsSnapshot::fromProto(
            std::string_view(reinterpret_cast<const char *>(bytes), sizeof(bytes))
        );
        REQUIRE_FALSE(res.success());
    }

    SECTION("Truncated length-delimited payload") {
        const uint8_t bytes[] = { 0x5a, 0x05, 0x01, 0x02 };
        const auto res = PreloadedSettingsSnapshot::fromProto(
            std::string_view(reinterpret_cast<const char *>(bytes), sizeof(bytes))
        );
        REQUIRE_FALSE(res.success());
    }

    SECTION("Deprecated start group wire type rejected") {
        const uint8_t bytes[] = { 0x0b, 0x01 };
        const auto res = PreloadedSettingsSnapshot::fromProto(
            std::string_view(reinterpret_cast<const char *>(bytes), sizeof(bytes))
        );
        REQUIRE_FALSE(res.success());
    }

    SECTION("Known group with non-length-delimited wire type rejected") {
        const uint8_t bytes[] = { 0x58, 0x01 };
        const auto res = PreloadedSettingsSnapshot::fromProto(
            std::string_view(reinterpret_cast<const char *>(bytes), sizeof(bytes))
        );
        REQUIRE_FALSE(res.success());
    }
}
