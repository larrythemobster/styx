#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Styx {
namespace Proto {

class ProtoReader;

struct FrecencyItem
{
    uint32_t totalUses = 0;
    std::vector<uint64_t> recentUses;
    int32_t frecency = 0;
    int32_t score = 0;

    static FrecencyItem fromProto(ProtoReader &reader);
};

struct EmojiFrecency
{
    std::unordered_map<std::string, FrecencyItem> emojis;

    static EmojiFrecency fromProto(ProtoReader &reader);
};

enum class GifType : uint8_t {
    None = 0,
    Image = 1,
    Video = 2,
};

struct FavoriteGif
{
    std::string url;
    GifType format = GifType::None;
    std::string src;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t order = 0;
    std::string messageAsReceived;
};

struct FavoriteGifs
{
    std::vector<FavoriteGif> gifs;
    bool hideTooltip = false;
    std::string unknownFields;

    static FavoriteGifs fromProto(std::span<const uint8_t> bytes);
    static FavoriteGifs fromProto(std::string_view bytes)
    {
        return fromProto(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
    }
    [[nodiscard]] std::string toProto() const;
};

// discord_protos.discord_users.v1.FrecencyUserSettings
struct FrecencyUserSettings
{
    // Versions.data_version, bumped by the server on every change
    std::optional<uint32_t> dataVersion;
    std::optional<FavoriteGifs> favoriteGifs;
    std::optional<EmojiFrecency> emojiFrecency;
    std::optional<EmojiFrecency> emojiReactionFrecency;

    static FrecencyUserSettings fromProto(ProtoReader &reader);

    std::string toProtoPartial() const;
};

} // namespace Proto
} // namespace Styx
