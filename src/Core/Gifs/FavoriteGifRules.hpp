#pragma once

#include <string>
#include <vector>

#include "Platform/Native.hpp"
#include "Proto/FrecencySettings.hpp"

namespace Styx::Core {

struct FavoriteGifCandidate
{
    std::string url;
    std::string src;
    std::string gifSrc;
    Proto::GifType format = Proto::GifType::Image;
    Platform::Size size;

    [[nodiscard]] bool isValid() const { return !url.empty() && !src.empty(); }
};

namespace FavoriteGifRules {

inline constexpr int OfficialClientMaxSerializedBytes = 762880;
inline constexpr int CountThatHidesTheTooltip = 2;

[[nodiscard]] std::string keyFor(const std::string &url);
[[nodiscard]] Proto::FavoriteGif stored(const FavoriteGifCandidate &candidate, uint32_t order);
[[nodiscard]] uint32_t orderAfter(const std::vector<Proto::FavoriteGif> &gifs);
[[nodiscard]] std::vector<Proto::FavoriteGif> newestFirst(std::vector<Proto::FavoriteGif> gifs);
[[nodiscard]] bool matchesSearch(const std::string &url, const std::string &query);
[[nodiscard]] std::string clipServedByDiscord(const Proto::FavoriteGif &gif);
[[nodiscard]] std::string stillServedByDiscord(const Proto::FavoriteGif &gif);

} // namespace FavoriteGifRules
} // namespace Styx::Core
