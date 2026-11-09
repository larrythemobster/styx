#pragma once

#include <charconv>
#include <cstdint>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "Core/Markdown/Parser.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"
#include "Platform/Native.hpp"
#include "RmlRuntime.hpp"
#include "Shell.hpp"

namespace Styx::UI::Rml {

struct RenderedMarkdown
{
    std::string rml;
    bool mentionsUser = false;
};

class ChatMarkdown
{
public:
    static RenderedMarkdown toRml(const std::string &source, Shell &shell, Core::Snowflake currentUserId, Core::Snowflake guildId, Core::Snowflake channelId)
    {
        RenderedMarkdown result;
        if (source.empty())
            return result;

        if (currentUserId.isValid()) {
            const auto id = currentUserId.toString();
            result.mentionsUser = source.find("<@" + id + ">") != std::string::npos ||
                                  source.find("<@!" + id + ">") != std::string::npos;
        }

        std::string processed = source;
        std::vector<std::pair<std::string, std::string>> codeBlocks;
        std::vector<std::pair<std::string, std::string>> spoilers;
        // Choose a prefix that cannot collide with literal message content.
        std::string prefix = "STYXPLACEHOLDER";
        while (source.find(prefix) != std::string::npos)
            prefix += 'X';

        static const std::regex codeRegex(R"(```(?:([a-zA-Z0-9_+-]+)?\n)?([\s\S]*?)```)");
        std::smatch match;
        while (std::regex_search(processed, match, codeRegex)) {
            auto placeholder = prefix + "CODE" + std::to_string(codeBlocks.size()) + "TAG";
            auto code = Platform::htmlEscape(match[2].str());
            codeBlocks.emplace_back(placeholder, "<pre class=\"code-block\"><code>" + code + "</code></pre>");
            processed.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), placeholder);
        }

        static const std::regex spoilerRegex(R"(\|\|([\s\S]+?)\|\|)");
        while (std::regex_search(processed, match, spoilerRegex)) {
            auto placeholder = prefix + "SPOILER" + std::to_string(spoilers.size()) + "TAG";
            spoilers.emplace_back(placeholder, match[1].str());
            processed.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()), placeholder);
        }

        Core::Markdown::Parser parser;
        Core::Markdown::ParseState state;
        state.isInline = true;
        state.customState["channelId"] = static_cast<std::uint64_t>(channelId);
        auto ast = parser.parse(std::move(processed), std::move(state));
        auto rml = renderNodes(ast, shell, Core::Markdown::Parser::isEmojiOnly(ast), result.mentionsUser, currentUserId, guildId);

        for (const auto &[placeholder, inner] : spoilers) {
            auto rendered = toRml(inner, shell, currentUserId, guildId, channelId);
            result.mentionsUser = result.mentionsUser || rendered.mentionsUser;
            replaceAll(rml, placeholder, "<span class=\"spoiler blurred\" data-action=\"reveal-text-spoiler\">" + rendered.rml + "</span>");
        }
        for (const auto &[placeholder, code] : codeBlocks)
            replaceAll(rml, placeholder, code);
        while (rml.ends_with("<br/>"))
            rml.resize(rml.size() - 5);
        result.rml = std::move(rml);
        return result;
    }

private:
    static void replaceAll(std::string &text, const std::string &needle, const std::string &replacement)
    {
        std::size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            text.replace(pos, needle.size(), replacement);
            pos += replacement.size();
        }
    }

    static Core::Snowflake snowflake(const std::string &text)
    {
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc{} && end == text.data() + text.size() ? Core::Snowflake(value) : Core::Snowflake();
    }

    static std::string renderNodes(const std::vector<Core::Markdown::AstNode> &nodes, Shell &shell, bool emojiOnly, bool &mentionsUser, Core::Snowflake currentUserId, Core::Snowflake guildId)
    {
        std::string out;
        for (const auto &node : nodes) {
            if (node.type == "text") {
                out += richText(node.content);
            } else if (node.type == "user") {
                auto userId = snowflake(node.content);
                auto *instance = shell.currentInstance();
                std::string name;
                if (instance && instance->users())
                    name = instance->users()->getDisplayName(userId, guildId);
                if (name.empty())
                    name = userId.toString();
                const bool isMe = userId == currentUserId;
                if (isMe)
                    mentionsUser = true;
                out += "<span class=\"mention mention-user" + std::string(isMe ? " mention-me" : "") + "\" data-action=\"user-mention\" data-user-id=\"" + userId.toString() + "\">@" + Platform::htmlEscape(name) + "</span>";
            } else if (node.type == "channel") {
                auto *instance = shell.currentInstance();
                std::string name;
                if (instance) {
                    if (auto channel = instance->getChannel(snowflake(node.content))) {
                        if (channel->name.hasValue())
                            name = *channel->name;
                    }
                }
                if (name.empty())
                    name = node.content;
                auto id = Platform::htmlEscape(node.content);
                out += "<a href=\"styx://channel/" + id + "\" class=\"mention mention-channel\" data-action=\"channel-mention\" data-channel-id=\"" + id + "\">#" + Platform::htmlEscape(name) + "</a>";
            } else if (node.type == "channelLink") {
                auto href = Platform::htmlEscape(node.attributes.value("href", std::string()));
                out += "<a href=\"" + href + "\" class=\"mention mention-channel\" data-action=\"channel-link\" data-href=\"" + href + "\">" + href + "</a>";
            } else if (node.type == "customEmoji") {
                auto name = Platform::htmlEscape(node.attributes.value("name", std::string()));
                bool animated = node.attributes.value("animated", false);
                int px = emojiOnly ? 48 : 22;
                auto url = Discord::Cdn::emoji(snowflake(node.content), px, animated);
                auto src = Platform::htmlEscape(shell.imageSource(url, Platform::Size{px, px}));
                out += "<img class=\"emoji" + std::string(emojiOnly ? " jumbo" : "") + "\" src=\"" + src + "\" title=\":" + name + ":\" alt=\":" + name + ":\" />";
            } else if (node.type == "strong" || node.type == "em" || node.type == "u" || node.type == "strike") {
                const auto tag = node.type == "strike" ? "s" : node.type.c_str();
                out += "<" + std::string(tag) + ">" + renderNodes(node.children, shell, false, mentionsUser, currentUserId, guildId) + "</" + tag + ">";
            } else if (node.type == "inlineCode") {
                out += "<code class=\"inline-code\">" + Platform::htmlEscape(node.content) + "</code>";
            } else if (node.type == "url" || node.type == "autolink" || node.type == "link") {
                auto href = node.attributes.value("href", std::string());
                if (href.empty() && node.type != "link")
                    href = node.content;
                auto text = node.type == "link" ? renderNodes(node.children, shell, false, mentionsUser, currentUserId, guildId) : Platform::htmlEscape(node.content);
                if (text.empty() && node.type == "link")
                    text = Platform::htmlEscape(href);
                href = Platform::htmlEscape(href);
                out += "<a href=\"" + href + "\" class=\"chat-link\" data-action=\"link\" data-href=\"" + href + "\">" + text + "</a>";
            } else if (node.type == "br" || node.type == "newline") {
                out += "<br/>";
            } else if (!node.children.empty()) {
                out += renderNodes(node.children, shell, emojiOnly, mentionsUser, currentUserId, guildId);
            } else if (!node.content.empty()) {
                out += Platform::htmlEscape(node.content);
            }
        }
        return out;
    }
};

} // namespace Styx::UI::Rml
