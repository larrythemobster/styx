#include "Core/Emoji/UnicodeEmojiIndex.hpp"

#include "Core/Logging.hpp"
#include "Platform/Native.hpp"

#include <algorithm>
#include <iterator>
#include <nlohmann/json.hpp>

namespace Styx::Core {
namespace {
constexpr const char *skinTones[] = { "1f3fb", "1f3fc", "1f3fd", "1f3fe", "1f3ff" };
int toneNumber(const std::string &codepoint)
{
    for (int i = 0; i < static_cast<int>(std::size(skinTones)); ++i)
        if (codepoint == skinTones[i]) return i + 1;
    return 0;
}
std::vector<std::string> strings(const nlohmann::json &object, const char *key)
{
    std::vector<std::string> result;
    auto it = object.find(key);
    if (it != object.end() && it->is_array()) {
        result.reserve(it->size());
        for (const auto &item : *it)
            result.push_back(item.is_string() ? item.get<std::string>() : std::string{});
    }
    return result;
}
}

const std::regex &customEmojiTagRe()
{
    static const std::regex re(R"(<a?:([a-zA-Z0-9_]{1,32}):(\d+)>)");
    return re;
}

UnicodeEmojiIndex &UnicodeEmojiIndex::instance()
{
    static UnicodeEmojiIndex index;
    return index;
}

UnicodeEmojiIndex::UnicodeEmojiIndex()
{
    const auto bytes = Platform::Resources::read("emoji_data.json");
    const auto doc = nlohmann::json::parse(bytes, nullptr, false);
    if (!doc.is_object() || !doc.contains("emojis") || !doc["emojis"].is_array()) {
        Core::log(Core::LogLevel::Warning, LogCore) << "UnicodeEmojiIndex: cannot read emoji_data.json";
        return;
    }
    const auto &entries = doc["emojis"];
    emojis.reserve(entries.size());
    for (const auto &obj : entries) {
        UnicodeEmoji emoji;
        if (obj.is_object()) {
            emoji.names = strings(obj, "names");
            emoji.keywords = strings(obj, "keywords");
            emoji.surrogates = obj.value("surrogates", std::string{});
            emoji.category = obj.value("category", std::string{});
            emoji.hasDiversity = obj.value("hasDiversity", false);
            emoji.isDiversityChild = obj.value("hasDiversityParent", false) || obj.value("hasMultiDiversityParent", false);
        }
        emojis.push_back(std::move(emoji));
    }
    for (size_t i = 0; i < emojis.size(); ++i) {
        const auto &emoji = emojis[i];
        for (const auto &name : emoji.names) nameToIndex[name] = static_cast<int>(i);
        surrogateToIndex[emoji.surrogates] = static_cast<int>(i);
        if (!emoji.isDiversityChild) {
            topLevelEmojis.push_back(&emoji);
            if (topLevelByCategory.empty() || topLevelByCategory.back().id != emoji.category)
                topLevelByCategory.push_back({emoji.category, {}});
            topLevelByCategory.back().emojis.push_back(&emoji);
        }
        if (!emoji.hasDiversity || !entries[i].contains("diversityChildren") || !entries[i]["diversityChildren"].is_array()) continue;
        for (const auto &child : entries[i]["diversityChildren"]) {
            if (!child.is_number_integer()) continue;
            int childIndex = child.get<int>();
            if (childIndex < 0 || static_cast<size_t>(childIndex) >= emojis.size()) continue;
            auto tones = strings(entries[childIndex], "diversity");
            int tone = tones.empty() ? 0 : toneNumber(tones.front());
            if (!tone) continue;
            const auto suffix = "::skin-tone-" + std::to_string(tone);
            for (const auto &name : emoji.names) nameToIndex[name + suffix] = childIndex;
        }
    }
    Core::log(Core::LogLevel::Debug, LogCore) << "UnicodeEmojiIndex: loaded " << emojis.size() << " emojis";
}

const UnicodeEmoji *UnicodeEmojiIndex::byName(const std::string &name) const
{
    auto it = nameToIndex.find(name);
    return it == nameToIndex.end() ? nullptr : &emojis[it->second];
}
const UnicodeEmoji *UnicodeEmojiIndex::bySurrogate(const std::string &surrogates) const
{
    auto it = surrogateToIndex.find(surrogates);
    return it == surrogateToIndex.end() ? nullptr : &emojis[it->second];
}
const UnicodeEmoji *UnicodeEmojiIndex::convertSurrogateToBase(const std::string &surrogates) const
{
    auto points = Platform::codepoints(surrogates);
    std::erase_if(points, [](char32_t cp) { return cp >= 0x1f3fb && cp <= 0x1f3ff; });
    return bySurrogate(Platform::fromCodepoints(points));
}
std::string UnicodeEmojiIndex::translateNamesToSurrogates(const std::string &text) const
{
    static const std::regex shortcode(R"(:([^\s:]+?(?:::skin-tone-\d)?):)");
    std::vector<std::pair<size_t, size_t>> tags;
    for (std::sregex_iterator it(text.begin(), text.end(), customEmojiTagRe()), end; it != end; ++it)
        tags.emplace_back(it->position(), it->position() + it->length());
    std::string result;
    size_t copied = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), shortcode), end; it != end; ++it) {
        size_t start = it->position(), finish = start + it->length();
        if (std::any_of(tags.begin(), tags.end(), [start, finish](const auto &range) { return start < range.second && finish > range.first; })) continue;
        const auto *emoji = byName((*it)[1].str());
        if (!emoji) continue;
        result.append(text, copied, start - copied);
        result += emoji->surrogates;
        copied = finish;
    }
    if (!copied) return text;
    result.append(text, copied, std::string::npos);
    return result;
}
} // namespace Styx::Core
