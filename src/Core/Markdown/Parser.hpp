#pragma once

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>
#include "Core/Snowflake.hpp"

namespace Styx::Core::Markdown {

inline constexpr int InlineEmojiPx = 22;
inline constexpr int JumboEmojiPx = 44;

struct AstNode {
    std::string type;
    std::string content;
    std::vector<AstNode> children;
    nlohmann::json attributes = nlohmann::json::object();
};
struct ParseState {
    bool isInline = false;
    bool inTable = false;
    int key = 0;
    std::string prevCapture;
    nlohmann::json customState = nlohmann::json::object();
    std::unordered_set<std::string> excludedRules;
};
using UserResolverFn = std::function<std::string(Snowflake, Snowflake)>;
using ChannelResolverFn = std::function<std::string(const std::string &)>;
struct ChannelLinkRef {
    Snowflake guildId;
    Snowflake channelId;
    Snowflake messageId;
    Snowflake sourceChannelId;
};
struct ChannelLinkPart {
    std::string icon;
    std::string text;
    bool italic = false;
};
using ChannelLinkResolverFn = std::function<std::vector<ChannelLinkPart>(const ChannelLinkRef &)>;

class Parser {
public:
    Parser() = default;
    std::vector<AstNode> parse(std::string source, ParseState state = {});
    std::string toHtml(const std::vector<AstNode> &nodes, bool jumboEmoji = false);
    void setUserResolver(UserResolverFn resolver);
    void setChannelResolver(ChannelResolverFn resolver);
    void setChannelLinkResolver(ChannelLinkResolverFn resolver);
    static bool isEmojiOnly(const std::vector<AstNode> &nodes, int maxEmojis = 30);
    static bool isSingleLink(const std::vector<AstNode> &nodes);
private:
    UserResolverFn userResolver;
    ChannelResolverFn channelResolver;
    ChannelLinkResolverFn channelLinkResolver;
};

} // namespace Styx::Core::Markdown
