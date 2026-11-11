#include "Parser.hpp"
#include "Core/EmojiSegmenter.hpp"
#include "Discord/ChannelLink.hpp"
#include "Discord/CdnUrls.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <regex>
#include <string_view>
#include <utility>

namespace Styx::Core::Markdown {
namespace {
constexpr int InlineEmojiCdnPx = 64;
constexpr int JumboEmojiCdnPx = 128;

std::uint64_t number(std::string_view text)
{
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : 0;
}

std::uint64_t attributeId(const nlohmann::json &attributes, const char *key)
{
    auto it = attributes.find(key);
    if (it == attributes.end() || it->is_null()) return 0;
    if (it->is_number_unsigned()) return it->get<std::uint64_t>();
    if (it->is_number_integer()) return static_cast<std::uint64_t>(it->get<std::int64_t>());
    if (it->is_string()) return number(it->get_ref<const std::string &>());
    return 0;
}

std::string attributeString(const nlohmann::json &attributes, const char *key)
{
    auto it = attributes.find(key);
    return it != attributes.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string escapeHtml(std::string_view text)
{
    std::string result;
    for (char c : text) {
        switch (c) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string normalizeLinkUrl(std::string url)
{
    std::string prefix = url.substr(0, 8);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto start = prefix.starts_with("https://") ? 8u : prefix.starts_with("http://") ? 7u : 0u;
    if (!start) return url;
    const auto end = url.find_first_of("/?#", start);
    if (end != start && (end == std::string::npos || url[end] != '/'))
        url.insert(end == std::string::npos ? url.size() : end, 1, '/');
    return url;
}

bool allowedAutolink(const std::string &target)
{
    auto colon = target.find(':');
    if (colon == std::string::npos) return false;
    auto scheme = target.substr(0, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (scheme != "http" && scheme != "https" && scheme != "discord" && scheme != "tel" && scheme != "sms" && scheme != "mailto") return false;
    if (scheme == "http" || scheme == "https") {
        if (target.compare(colon + 1, 2, "//") != 0) return false;
        const auto host = colon + 3;
        if (host >= target.size() || target.find_first_of("/?#", host) == host) return false;
    }
    return true;
}

struct Rule {
    const char *name;
    int order;
    enum Scope { Any, Inline, Block } scope;
    std::regex regex;
};

const std::vector<Rule> &rules()
{
    static const std::vector<Rule> value = {
        {"newline", 10, Rule::Block, std::regex(R"(^(?:\n *)*\n)")},
        {"escape", 12, Rule::Inline, std::regex(R"(^\\([^0-9A-Za-z\s]))")},
        {"autolink", 15, Rule::Inline, std::regex(R"(^<([^: >]+:\/[^ >]+)>)")},
        {"channelLink", 16, Rule::Inline, Discord::ChannelLink::pattern(R"((?=$|[\s<.,:;"'!?)\]]))")},
        {"url", 17, Rule::Inline, std::regex(R"(^(https?:\/\/[^\s<]+[^<.,:;"'\]\s]))")},
        {"link", 18, Rule::Inline, std::regex(R"(^\[((?:\[[^\]]*\]|[^\[\]]|\](?=[^\[]*\]))*)\]\(\s*<?((?:\([^)]*\)|[^\s\\]|\\.)*?)>?(?:\s+['"]([\s\S]*?)['"])?\s*\))")},
        {"em", 20, Rule::Inline, std::regex(R"(^\b_((?:__|\\[\s\S]|[^\\_])+?)_\b|^\*(?=\S)((?:\*\*|\\[\s\S]|\s+(?:\\[\s\S]|[^\s\*\\]|\*\*)|[^\s\*\\])+?)\*(?!\*))")},
        {"user", 21, Rule::Any, std::regex(R"(^<@!?([0-9]*)>)")},
        {"channel", 21, Rule::Any, std::regex(R"(^<#?([0-9]*)>)")},
        {"customEmoji", 21, Rule::Any, std::regex(R"(^<(a?):([a-zA-Z0-9_]{1,32}):(\d+)>)")},
        {"strong", 21, Rule::Inline, std::regex(R"(^\*\*((?:\\[\s\S]|[^\\])+?)\*\*(?!\*))")},
        {"u", 21, Rule::Inline, std::regex(R"(^__((?:\\[\s\S]|[^\\])+?)__(?!_))")},
        {"strike", 22, Rule::Inline, std::regex(R"(^~~([\s\S]+?)~~(?!_))")},
        {"inlineCode", 23, Rule::Inline, std::regex(R"(^(`+)([\s\S]*?[^`])\1(?!`))")},
        {"br", 24, Rule::Any, std::regex(R"(^\n)")}
    };
    return value;
}

bool textBoundary(const std::string &source, std::size_t pos)
{
    const unsigned char c = static_cast<unsigned char>(source[pos]);
    if (c == '\n') return true;
    if (c < 128 && !std::isalnum(c) && !std::isspace(c) && c != '-') return true;
    if (c < 128 && (std::isalnum(c) || c == '_')) {
        auto end = pos;
        while (end < source.size()) {
            const unsigned char ch = static_cast<unsigned char>(source[end]);
            if (ch >= 128 || (!std::isalnum(ch) && ch != '_')) break;
            ++end;
        }
        if (end + 1 < source.size() && source[end] == ':' && !std::isspace(static_cast<unsigned char>(source[end + 1]))) return true;
    }
    return false;
}
} // namespace

std::vector<AstNode> Parser::parse(std::string source, ParseState state)
{
    std::string normalized;
    normalized.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\r') {
            normalized += '\n';
            if (i + 1 < source.size() && source[i + 1] == '\n') ++i;
        } else if (source[i] == '\t') normalized += "    ";
        else normalized += source[i];
    }
    source = std::move(normalized);
    if (!state.isInline) source += "\n\n";

    std::vector<AstNode> result;
    while (!source.empty()) {
        const Rule *best = nullptr;
        std::smatch capture, match;
        double quality = 0;
        bool regexFailure = false;
        for (const auto &rule : rules()) {
            if (state.excludedRules.contains(rule.name) || (rule.scope == Rule::Inline && !state.isInline) || (rule.scope == Rule::Block && state.isInline)) continue;
            if (best && rule.order > best->order) break;
            const auto matchEnd = source.cend();
            try {
                if (!std::regex_search(source.cbegin(), matchEnd, match, rule.regex, std::regex_constants::match_continuous) || match.length() == 0) continue;
            } catch (const std::regex_error &) {
                regexFailure = true;
                break;
            }
            double candidate = 0;
            if (std::string_view(rule.name) == "strong") candidate = match.length() + 0.1;
            else if (std::string_view(rule.name) == "u") candidate = static_cast<double>(match.length());
            if (!best || candidate >= quality) { best = &rule; capture = match; quality = candidate; }
        }
        AstNode node;
        std::size_t consumed = 0;
        if (regexFailure) {
            // Leave the input literal instead of aborting the UI.
            consumed = source.size();
            node.type = "text";
            node.content = source;
        } else if (!best) {
            // The text rule consumes at least one character before checking its lookahead.
            consumed = 1;
            while (consumed < source.size() && (static_cast<unsigned char>(source[consumed]) & 0xc0) == 0x80) ++consumed;
            if (!state.excludedRules.contains("text")) {
                while (consumed < source.size() && !textBoundary(source, consumed)) ++consumed;
            }
            node.type = "text";
            node.content = source.substr(0, consumed);
        } else {
            const std::size_t matchedLength = capture.length();
            node.type = best->name;
            consumed = matchedLength;
            const std::string_view type = best->name;
            if (type == "escape") { node.type = "text"; node.content = capture[1].str(); }
            else if (type == "autolink") {
                node.content = capture[1].str();
                if (allowedAutolink(node.content)) { node.type = "url"; node.content = normalizeLinkUrl(node.content); node.attributes["href"] = node.content; }
                else node.type = "text";
            } else if (type == "channelLink") {
                const auto link = Discord::ChannelLink::fromMatch(capture);
                node.content = capture[0].str();
                node.attributes["href"] = node.content;
                node.attributes["guildId"] = static_cast<std::uint64_t>(link.guildId);
                node.attributes["channelId"] = static_cast<std::uint64_t>(link.channelId);
                node.attributes["messageId"] = static_cast<std::uint64_t>(link.messageId);
                node.attributes["sourceChannelId"] = attributeId(state.customState, "channelId");
            } else if (type == "url") {
                auto target = capture[1].str();
                std::size_t search = 0;
                for (std::size_t i = target.size(); i > 0 && target[i - 1] == ')'; --i) {
                    auto open = target.find('(', search);
                    if (open == std::string::npos) { target.pop_back(); --consumed; break; }
                    search = open + 1;
                }
                node.content = normalizeLinkUrl(target);
                node.attributes["href"] = node.content;
            } else if (type == "link") {
                node.children = parse(capture[1].str(), state);
                node.attributes["href"] = capture[2].str();
                node.attributes["title"] = capture[3].str();
            } else if (type == "em" || type == "strong" || type == "u" || type == "strike") {
                state.isInline = true;
                node.children = parse(type == "em" && capture[2].matched ? capture[2].str() : capture[1].str(), state);
            } else if (type == "user" || type == "channel") {
                node.content = capture[1].str();
                if (type == "user") node.attributes["sourceChannelId"] = attributeId(state.customState, "channelId");
            } else if (type == "customEmoji") {
                node.content = capture[3].str();
                node.attributes["name"] = capture[2].str();
                node.attributes["animated"] = !capture[1].str().empty();
            } else if (type == "inlineCode") {
                node.content = capture[2].str();
                static const std::regex spaces(R"(^ (?= *`)|(` *) $)");
                node.content = std::regex_replace(node.content, spaces, "$1");
            }
        }
        state.prevCapture = source.substr(0, consumed);
        source.erase(0, consumed);
        result.push_back(std::move(node));
    }
    return result;
}

std::string Parser::toHtml(const std::vector<AstNode> &nodes, bool jumboEmoji)
{
    std::string result;
    for (const auto &node : nodes) {
        const auto &type = node.type;
        if (type == "user") {
            auto name = userResolver ? userResolver(Snowflake(number(node.content)), Snowflake(attributeId(node.attributes, "sourceChannelId"))) : node.content;
            result += "<span class=\"mention\">@" + escapeHtml(name) + "</span>";
        } else if (type == "channel") {
            auto name = channelResolver ? channelResolver(node.content) : node.content;
            result += "<a href=\"styx://channel/" + escapeHtml(node.content) + "\" class=\"mention\">#" + escapeHtml(name) + "</a>";
        } else if (type == "channelLink") {
            auto href = escapeHtml(attributeString(node.attributes, "href"));
            std::vector<ChannelLinkPart> parts;
            if (channelLinkResolver) parts = channelLinkResolver({Snowflake(attributeId(node.attributes, "guildId")), Snowflake(attributeId(node.attributes, "channelId")), Snowflake(attributeId(node.attributes, "messageId")), Snowflake(attributeId(node.attributes, "sourceChannelId"))});
            if (parts.empty()) result += "<a href=\"" + href + "\">" + href + "</a>";
            else {
                auto icon = [](const std::string &name) { return "<img src=\"styx-icon:mention-" + name + "\" width=\"14\" height=\"14\" style=\"vertical-align: middle\" />"; };
                std::string inner;
                for (std::size_t i = 0; i < parts.size(); ++i) {
                    if (i) inner += icon("chevron");
                    if (!parts[i].icon.empty()) inner += icon(parts[i].icon);
                    auto text = escapeHtml(parts[i].text);
                    inner += parts[i].italic ? "<i>" + text + "</i>" : text;
                }
                result += "<a href=\"" + href + "\" class=\"mention\">" + inner + "</a>";
            }
        } else if (type == "customEmoji") {
            const auto size = jumboEmoji ? JumboEmojiPx : InlineEmojiPx;
            const auto cdnSize = jumboEmoji ? JumboEmojiCdnPx : InlineEmojiCdnPx;
            auto animated = node.attributes.value("animated", false);
            auto url = Discord::Cdn::emoji(Snowflake(number(node.content)), cdnSize, animated);
            result += "<img src=\"" + escapeHtml(url) + "\" alt=\":" + escapeHtml(attributeString(node.attributes, "name")) + ":\" width=\"" + std::to_string(size) + "\" height=\"" + std::to_string(size) + "\" style=\"vertical-align: middle;\" />";
        } else if (type == "text" || type == "escape") {
            auto text = escapeHtml(node.content);
            if (jumboEmoji && type == "text" && node.content.find_first_not_of(" \t\r\n\f\v") != std::string::npos) result += "<span style=\"font-size: " + std::to_string(JumboEmojiPx) + "px;\">" + text + "</span>";
            else result += text;
        } else if (type == "url" || type == "link") {
            result += "<a href=\"" + escapeHtml(attributeString(node.attributes, "href")) + "\">" + (type == "link" ? toHtml(node.children, jumboEmoji) : escapeHtml(node.content)) + "</a>";
        } else if (type == "em" || type == "strong" || type == "u" || type == "strike") {
            const auto tag = type == "strike" ? "s" : type.c_str();
            result += "<" + std::string(tag) + ">" + toHtml(node.children, jumboEmoji) + "</" + tag + ">";
        } else if (type == "inlineCode") result += "<code>" + escapeHtml(node.content) + "</code>";
        else if (type == "br") result += "<br>";
        else if (type == "newline") result += '\n';
        else result += node.content;
    }
    return result;
}

bool Parser::isEmojiOnly(const std::vector<AstNode> &nodes, int maxEmojis)
{
    int total = 0;
    for (const auto &node : nodes) {
        if (node.type == "customEmoji") ++total;
        else if (node.type == "br" || node.type == "newline") continue;
        else if (node.type == "text" || node.type.empty()) {
            const int count = countUnicodeEmojisSegmented(node.content);
            if (count < 0) return false;
            total += count;
        } else return false;
        if (total > maxEmojis) return false;
    }
    return total > 0;
}

bool Parser::isSingleLink(const std::vector<AstNode> &nodes)
{
    return nodes.size() == 1 && (nodes.front().type == "url" || nodes.front().type == "link");
}

void Parser::setUserResolver(UserResolverFn resolver) { userResolver = std::move(resolver); }
void Parser::setChannelResolver(ChannelResolverFn resolver) { channelResolver = std::move(resolver); }
void Parser::setChannelLinkResolver(ChannelLinkResolverFn resolver) { channelLinkResolver = std::move(resolver); }

} // namespace Styx::Core::Markdown
