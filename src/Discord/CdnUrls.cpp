#include "CdnUrls.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <string_view>
#include <unordered_map>

namespace Styx::Discord::Cdn {
namespace {
constexpr std::array sizes = {16,20,22,24,28,32,40,44,48,56,60,64,80,96,100,128,160,240,256,300,320,480,512,600,640,1024,1280,1536,2048,3072,4096};
std::string lower(std::string s) { for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
bool extension(const std::string &url, std::string_view ext) {
    auto s = lower(url); size_t pos = 0;
    while ((pos = s.find(ext, pos)) != std::string::npos) {
        auto end = pos + ext.size();
        if (end == s.size() || s[end] == '?' || s[end] == '#') return true;
        ++pos;
    }
    return false;
}
bool https(const std::string &url) { return lower(url.substr(0, 8)) == "https://"; }
std::string queryValue(const std::string &url, std::string_view name) {
    auto start = url.find('?'); if (start == std::string::npos) return {};
    auto end = url.find('#', ++start);
    std::string_view q(url.data() + start, (end == std::string::npos ? url.size() : end) - start);
    while (!q.empty()) {
        auto amp = q.find('&'); auto part = q.substr(0, amp); auto eq = part.find('=');
        if (part.substr(0, eq) == name) return eq == std::string_view::npos ? std::string() : Platform::urlDecode(part.substr(eq + 1));
        if (amp == std::string_view::npos) break; q.remove_prefix(amp + 1);
    }
    return {};
}
std::string addQuery(std::string url, const std::string &item) {
    auto hash = url.find('#'); auto suffix = hash == std::string::npos ? std::string() : url.substr(hash);
    if (hash != std::string::npos) url.erase(hash);
    url += url.find('?') == std::string::npos ? '?' : '&';
    return url + item + suffix;
}
std::string mediaSticker(Core::Snowflake id, const char *ext, int px, const char *extra = "") {
    return "https://media.discordapp.net/stickers/" + id.toString() + "." + ext + "?size=" + std::to_string(px) + extra;
}
std::string asset(const char *path, Core::Snowflake id, const std::string &hash, int size) {
    if (!id.isValid() || hash.empty()) return {};
    return std::string("https://cdn.discordapp.com/") + path + "/" + id.toString() + "/" + hash + "." + assetExtension(hash) + "?size=" + std::to_string(size);
}
int64_t expiry(const std::string &url) {
    auto ex = queryValue(url, "ex"); int64_t value = 0;
    auto r = std::from_chars(ex.data(), ex.data() + ex.size(), value, 16);
    return r.ec == std::errc{} && r.ptr == ex.data() + ex.size() ? value : 0;
}
}
std::string assetExtension(const std::string &hash) { return hash.starts_with("a_") ? "gif" : "png"; }
std::string defaultUserAvatar(Core::Snowflake id, int) { return "https://cdn.discordapp.com/embed/avatars/" + std::to_string((static_cast<uint64_t>(id) >> 22) % 6) + ".png"; }
std::string userAvatar(Core::Snowflake id, const std::string &hash, int size) { return asset("avatars", id, hash, size); }
std::string guildMemberAvatar(Core::Snowflake guild, Core::Snowflake user, const std::string &hash, int size) {
    if (!guild.isValid() || !user.isValid() || hash.empty()) return {};
    return "https://cdn.discordapp.com/guilds/" + guild.toString() + "/users/" + user.toString() + "/avatars/" + hash + "." + assetExtension(hash) + "?size=" + std::to_string(size);
}
std::string effectiveAvatar(Core::Snowflake guild, Core::Snowflake user, const std::string &member, const std::string &hash, int size) {
    if (guild.isValid() && !member.empty()) return guildMemberAvatar(guild, user, member, size);
    return hash.empty() ? defaultUserAvatar(user, size) : userAvatar(user, hash, size);
}
std::string userBanner(Core::Snowflake id, const std::string &hash, int size) { return asset("banners", id, hash, size); }
std::string guildIcon(Core::Snowflake id, const std::string &hash, int size) { return asset("icons", id, hash, size); }
std::string guildBanner(Core::Snowflake id, const std::string &hash, int size) { return asset("banners", id, hash, size); }
std::string guildSplash(Core::Snowflake id, const std::string &hash, int size) { return asset("splashes", id, hash, size); }
std::string roleIcon(Core::Snowflake id, const std::string &hash, int size) { return asset("role-icons", id, hash, size); }
std::string applicationIcon(Core::Snowflake id, const std::string &hash, int size) { return asset("app-icons", id, hash, size); }
std::string channelIcon(Core::Snowflake id, const std::string &hash, int size) { return asset("channel-icons", id, hash, size); }
std::string badgeIcon(const std::string &hash, int size) { return hash.empty() ? std::string() : "https://cdn.discordapp.com/badge-icons/" + hash + ".png?size=" + std::to_string(size); }
std::string emoji(Core::Snowflake id, int size, bool animated) { return id.isValid() ? "https://cdn.discordapp.com/emojis/" + id.toString() + ".webp?size=" + std::to_string(size) + (animated ? "&animated=true" : "") : std::string(); }
bool isDiscordMediaUrl(const std::string &url) { auto host = lower(Platform::urlHost(url)); return https(url) && (host == "cdn.discordapp.com" || host == "media.discordapp.net"); }
bool isEmojiUrl(const std::string &url) { return isDiscordMediaUrl(url) && Platform::urlPath(url).starts_with("/emojis/"); }
bool isAnimatedEmojiUrl(const std::string &url) { auto q = url.find('?'); return isEmojiUrl(url) && q != std::string::npos && url.substr(q).find("animated=true") != std::string::npos; }
std::string stillEmojiUrl(const std::string &url) {
    auto q = url.find('?'); if (q == std::string::npos) return url;
    auto hash = url.find('#', q); auto query = url.substr(q + 1, (hash == std::string::npos ? url.size() : hash) - q - 1);
    std::string result = url.substr(0, q); size_t pos = 0; bool first = true;
    while (pos <= query.size()) { auto end = query.find('&', pos); auto part = query.substr(pos, end - pos);
        if (part.substr(0, part.find('=')) != "animated") { result += first ? '?' : '&'; result += part; first = false; }
        if (end == std::string::npos) break; pos = end + 1;
    }
    if (hash != std::string::npos) result += url.substr(hash); return result;
}
bool isStickerUrl(const std::string &url) { return isDiscordMediaUrl(url) && Platform::urlPath(url).starts_with("/stickers/"); }
Core::Snowflake stickerIdFromUrl(const std::string &url) {
    if (!isStickerUrl(url)) return {}; auto path = Platform::urlPath(url); auto slash = path.rfind('/'); auto name = path.substr(slash + 1); auto dot = name.rfind('.'); if (dot != std::string::npos) name.erase(dot);
    uint64_t id = 0; auto r = std::from_chars(name.data(), name.data() + name.size(), id); return r.ec == std::errc{} && r.ptr == name.data() + name.size() ? Core::Snowflake(id) : Core::Snowflake();
}
int stickerAssetPx(int logicalPx, double ratio) { int wanted = static_cast<int>(std::round(logicalPx * std::min(2.0, ratio))); auto it = std::find_if(sizes.begin(), sizes.end(), [wanted](int n){ return wanted <= n; }); return it == sizes.end() ? sizes.back() : *it; }
std::string stickerStill(Core::Snowflake id, StickerFormatType format, int px) {
    if (!id.isValid() || format == StickerFormatType::LOTTIE) return {};
    if (format == StickerFormatType::APNG) return mediaSticker(id, "png", px, "&passthrough=false");
    return mediaSticker(id, "webp", px, "&quality=lossless");
}
std::string stickerAnimated(Core::Snowflake id, StickerFormatType format, int px) {
    if (!id.isValid()) return {};
    switch (format) { case StickerFormatType::APNG: return mediaSticker(id, "png", px); case StickerFormatType::GIF: return mediaSticker(id, "gif", px); case StickerFormatType::LOTTIE: return "https://discord.com/stickers/" + id.toString() + ".json"; case StickerFormatType::PNG: return {}; } return {};
}
bool isSigned(const std::string &url) { return isDiscordMediaUrl(url) && expiry(url) > 0; }
bool hasExpired(const std::string &url) { return isSigned(url) && std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 3600 >= expiry(url); }
bool isDiscordAssetUrl(const std::string &url) { auto host = lower(Platform::urlHost(url)); return https(url) && (host == "cdn.discordapp.com" || host.ends_with(".discordapp.net")); }
bool isAnimatedImage(const std::string &url, bool animated) { return extension(url, ".gif") || (animated && (extension(url, ".webp") || extension(url, ".avif"))); }
std::string animatedImageUrl(const std::string &proxy, bool animated, const Platform::Size &source, const Platform::Size &target) {
    if (lower(Platform::urlHost(proxy)) == "cdn.discordapp.com") return proxy;
    auto result = proxy; bool avif = extension(proxy, ".avif");
    if (animated && (avif || extension(proxy, ".webp"))) result = addQuery(result, "animated=true");
    if (avif) result = addQuery(result, "format=webp");
    auto requested = target;
    if (!requested.empty()) {
        double scale = requested.width > 4096 ? 4096.0 / requested.width : 1.0;
        requested = {static_cast<int>(std::round(requested.width * scale)), static_cast<int>(std::round(requested.height * scale))};
        scale = requested.height > 4096 ? 4096.0 / requested.height : 1.0;
        requested = {static_cast<int>(std::round(requested.width * scale)), static_cast<int>(std::round(requested.height * scale))};
    }
    if (!requested.empty() && requested != source) { result = addQuery(result, "width=" + std::to_string(requested.width)); result = addQuery(result, "height=" + std::to_string(requested.height)); }
    return result;
}
bool fitsLoopingVideoLimit(const Platform::Size &size) { return (size.width <= 6016 && size.height <= 3384) || (size.width <= 3384 && size.height <= 6016); }
std::string activityAsset(Core::Snowflake id, const std::string &key, int size) {
    if (key.empty()) return {}; auto sep = key.find(':');
    if (sep != std::string::npos && sep > 0) {
        auto scheme = key.substr(0, sep), value = key.substr(sep + 1); if (value.empty()) return {};
        std::string encoded; size_t pos = 0; while (pos < value.size()) { auto slash = value.find('/', pos); encoded += Platform::urlEncode(std::string_view(value).substr(pos, slash - pos)); if (slash == std::string::npos) break; encoded += '/'; pos = slash + 1; }
        if (scheme == "mp") return "https://media.discordapp.net/" + encoded + (lower(value).ends_with(".gif") ? "?format=webp&animated=true" : "");
        if (scheme == "spotify") return "https://i.scdn.co/image/" + encoded;
        if (scheme == "youtube") return "https://i.ytimg.com/vi/" + encoded + "/hqdefault_live.jpg";
        if (scheme == "twitch") return "https://static-cdn.jtvnw.net/previews-ttv/live_user_" + encoded + "-" + std::to_string(size) + "x" + std::to_string(size) + ".jpg";
        return {};
    }
    if (!id.isValid() || key.find('/') != std::string::npos || key.find("..") != std::string::npos) return {};
    return "https://cdn.discordapp.com/app-assets/" + id.toString() + "/" + Platform::urlEncode(key) + ".png?size=" + std::to_string(size);
}
std::string connectionIcon(const std::string &type) {
    static const std::unordered_map<std::string, std::string> table = {
        {"twitch","45900de231038a6d"},{"youtube","c569f6e10f1fa390"},{"spotify","40518c6ced9fed93"},{"github","732989e3c998e808"},{"twitter","afaf5671c31d4941"},{"reddit","afc3f4fa75178004"},{"steam","07ca603d9273b502"},{"xbox","09781ebf5509f48e"},{"playstation","46a23515a87620b9"},{"facebook","bda550439e2a1af0"},{"battlenet","0e1a419c00a471a2"},{"bluesky","8a42ef6bc4dd4e0d"},{"bungie","a35b652323167799"},{"leagueoflegends","fc30b78cd5e1db04"},{"epicgames","ecf035f3cf08a860"},{"riotgames","fa9e14fc0fd3680f"},{"roblox","5d078bf414a0124c"},{"tiktok","d585ec74cbd4c723"},{"instagram","ea0692691904fca9"},{"paypal","81a39a4672d62d60"},{"ebay","d0f7fa72a2ca6388"},{"mastodon","53fcd6d102d44c2b"},{"crunchyroll","4e56bb1ecb9b9dc3"},{"skype","4f2cb7753e5d5e61"},{"samsung","d50e7459b395e5d0"},{"domain","fe9b03f9b097e2a9"},{"amazon-music","cda6fb74e92e32f1"}};
    auto it = table.find(type); return it == table.end() ? std::string() : "https://discord.com/assets/" + it->second + ".png";
}
}
