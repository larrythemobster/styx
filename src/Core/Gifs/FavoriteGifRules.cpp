#include "Core/Gifs/FavoriteGifRules.hpp"

#include <algorithm>
#include <initializer_list>
#include <regex>
#include <string_view>
#include <utility>

#include "Discord/CdnUrls.hpp"

namespace Styx::Core::FavoriteGifRules {
namespace {

bool isAnimatedImageFile(const std::string &url)
{
    static const std::regex pattern(R"(\.(webp|avif|gif)(\?|$))", std::regex::icase);
    return std::regex_search(url, pattern);
}

bool isVideoFile(const std::string &url)
{
    static const std::regex pattern(R"(\.(mp4|webm)(\?|$))", std::regex::icase);
    return std::regex_search(url, pattern);
}

bool hasScheme(const std::string &url)
{
    const auto colon = url.find(':');
    return colon != std::string::npos && colon > 0 && colon < url.find_first_of("/?#");
}

bool isAttachmentOnDiscord(const std::string &url)
{
    const auto host = Platform::urlHost(url);
    const auto path = Platform::urlPath(url);
    return (host == "cdn.discordapp.com" || host == "media.discordapp.net") &&
           (path.starts_with("/attachments/") || path.starts_with("/ephemeral-attachments/"));
}

bool isHostedByDiscord(const std::string &url)
{
    const auto host = Platform::urlHost(url);
    return hasScheme(url) && (isAttachmentOnDiscord(url) ||
        (host.starts_with("images-ext-") && host.ends_with(".discordapp.net") && Platform::urlPath(url).starts_with("/external/")));
struct QueryItem {
    std::string name;
    std::string value;
    bool hasEquals = false;
};
using QueryItems = std::vector<QueryItem>;

using QueryItems = std::vector<std::pair<std::string, std::string>>;

std::string editQuery(const std::string &url, std::initializer_list<std::string_view> remove,
                      const QueryItems &set = {})
{
    const auto fragment = url.find('#');
    const auto question = url.find('?');
    const bool hasQuery = question != std::string::npos && (fragment == std::string::npos || question < fragment);
    const auto end = fragment == std::string::npos ? url.size() : fragment;
    QueryItems items;
    if (hasQuery) {
        size_t start = question + 1;
        while (start < end) {
            const auto amp = url.find('&', start);
            const auto stop = std::min(amp == std::string::npos ? end : amp, end);
            const auto equals = url.find('=', start);
            const bool hasEquals = equals < stop;
            auto name = url.substr(start, (hasEquals ? equals : stop) - start);
            auto value = hasEquals ? url.substr(equals + 1, stop - equals - 1) : std::string{};
                items.push_back({std::move(name), std::move(value), hasEquals});
                std::find(remove.begin(), remove.end(), name) == remove.end())
                items.emplace_back(std::move(name), std::move(value));
            start = stop + 1;
    for (const auto &[name, value, unused] : set) {
        auto first = std::find_if(items.begin(), items.end(), [&](const auto &item) { return item.name == name; });
    for (const auto &[name, value] : set) {
            items.push_back({name, value, true});
        if (first == items.end()) {
            first->value = value;
            first->hasEquals = true;
            items.erase(std::remove_if(first + 1, items.end(), [&](const auto &item) { return item.name == name; }), items.end());
            first->second = value;
            items.erase(std::remove_if(first + 1, items.end(), [&](const auto &item) { return item.first == name; }), items.end());
        }
    }
    for (const auto &[name, value, hasEquals] : items) {
    bool first = true;
    for (const auto &[name, value] : items) {
        result += first ? '?' : '&';
        if (hasEquals) {
        result += name;
        if (!value.empty()) {
            result += '=';
            result += value;
        }
    }
    if (fragment != std::string::npos)
        result += url.substr(fragment);
    return result;
}

std::string asAnimatedWebp(const std::string &url)
{
    if (!hasScheme(url))
        return url;
    const auto path = Platform::unicodeLower(Platform::urlPath(url));
    const bool conversion = path.ends_with(".avif") || path.ends_with(".gif");
    if (!conversion && !path.ends_with(".webp"))
        return url;
    return conversion ? editQuery(url, {}, {{"format", "webp"}, {"animated", "true"}})
                      : editQuery(url, {}, {{"animated", "true"}});
}

std::string withoutSearchNoise(const std::string &text, bool spaces)
{
    auto result = Platform::unicodeLower(text);
    result.erase(std::remove_if(result.begin(), result.end(), [spaces](char c) {
        return c == '-' || c == '_' || (spaces && c == ' ');
    }), result.end());
    return result;
}

} // namespace

std::string keyFor(const std::string &url)
{
    return hasScheme(url) && isAttachmentOnDiscord(url) ? editQuery(url, {"ex", "is", "hm"}) : url;
}

Proto::FavoriteGif stored(const FavoriteGifCandidate &candidate, uint32_t order)
{
    const bool srcOnDiscord = isHostedByDiscord(candidate.src);
    const bool hasOtherGifSrc = !candidate.gifSrc.empty() && candidate.gifSrc != candidate.src;
    const bool preferGifSrc = (isVideoFile(candidate.src) && hasOtherGifSrc) || (srcOnDiscord && !candidate.gifSrc.empty());
    std::string src = preferGifSrc ? candidate.gifSrc : candidate.src;
    if (srcOnDiscord && isAnimatedImageFile(src))
        src = asAnimatedWebp(src);
    if (src.starts_with("//"))
        src.insert(0, "https:");

    Proto::FavoriteGif gif;
    gif.url = keyFor(candidate.url);
    gif.src = src;
    gif.format = isAnimatedImageFile(src) ? Proto::GifType::Image : candidate.format;
    gif.width = uint32_t(std::max(0, candidate.size.width));
    gif.height = uint32_t(std::max(0, candidate.size.height));
    gif.order = order;
    return gif;
}

uint32_t orderAfter(const std::vector<Proto::FavoriteGif> &gifs)
{
    uint32_t highest = 0;
    for (const auto &gif : gifs)
        highest = std::max(highest, gif.order);
    return highest + 1;
}

std::vector<Proto::FavoriteGif> newestFirst(std::vector<Proto::FavoriteGif> gifs)
{
    std::stable_sort(gifs.begin(), gifs.end(), [](const auto &a, const auto &b) { return a.order < b.order; });
    std::reverse(gifs.begin(), gifs.end());
    return gifs;
}

bool matchesSearch(const std::string &url, const std::string &query)
{
    return withoutSearchNoise(url, false).find(withoutSearchNoise(query, true)) != std::string::npos;
}

std::string clipServedByDiscord(const Proto::FavoriteGif &gif)
{
    return Discord::Cdn::isDiscordAssetUrl(gif.src) ? asAnimatedWebp(gif.src) : std::string{};
}

std::string stillServedByDiscord(const Proto::FavoriteGif &gif)
{
    return Discord::Cdn::isDiscordAssetUrl(gif.src) ? editQuery(gif.src, {"animated", "format"}) : std::string{};
}

} // namespace Styx::Core::FavoriteGifRules
