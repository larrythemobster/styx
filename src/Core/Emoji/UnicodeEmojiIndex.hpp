#pragma once

#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Styx::Core {

// `<:name:id>` / `<a:name:id>`.
const std::regex &customEmojiTagRe();

struct UnicodeEmoji {
    std::vector<std::string> names;
    std::vector<std::string> keywords;
    std::string surrogates;
    std::string category;
    bool hasDiversity = false;
    bool isDiversityChild = false;
};

struct UnicodeEmojiCategory {
    std::string id;
    std::vector<const UnicodeEmoji *> emojis;
};

class UnicodeEmojiIndex {
public:
    static UnicodeEmojiIndex &instance();
    [[nodiscard]] const UnicodeEmoji *byName(const std::string &name) const;
    [[nodiscard]] const UnicodeEmoji *bySurrogate(const std::string &surrogates) const;
    [[nodiscard]] const UnicodeEmoji *convertSurrogateToBase(const std::string &surrogates) const;
    [[nodiscard]] static std::string primaryName(const UnicodeEmoji &emoji) { return emoji.names.empty() ? std::string{} : emoji.names.front(); }
    [[nodiscard]] const std::vector<const UnicodeEmoji *> &topLevel() const { return topLevelEmojis; }
    [[nodiscard]] const std::vector<UnicodeEmojiCategory> &categories() const { return topLevelByCategory; }
    [[nodiscard]] std::string translateNamesToSurrogates(const std::string &text) const;
private:
    UnicodeEmojiIndex();
    std::vector<UnicodeEmoji> emojis;
    std::unordered_map<std::string, int> nameToIndex;
    std::unordered_map<std::string, int> surrogateToIndex;
    std::vector<const UnicodeEmoji *> topLevelEmojis;
    std::vector<UnicodeEmojiCategory> topLevelByCategory;
};

} // namespace Styx::Core
