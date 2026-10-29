#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "Platform/Trackable.hpp"

#include <functional>
#include <optional>

#include "Discord/Entities.hpp"
#include "Result.hpp"
#include "Snowflake.hpp"

namespace Styx {

namespace Discord {
class Client;
}

namespace Core {

class UserManager;

struct ReactionRef
{
    Snowflake messageId;
    Discord::Emoji emoji;
    bool isBurst = false;

    [[nodiscard]] bool sameReaction(const ReactionRef &other) const;
};

class MessageReactors : public Platform::Trackable
{
public:
    static constexpr int TooltipSample = 3;
    static constexpr int PageSize = 100;

    MessageReactors(Discord::Client *client, UserManager *users);

    [[nodiscard]] std::vector<Discord::User> reactors(Snowflake channelId, const ReactionRef &reaction, int firstReadLimit);

    using PageFetched = std::function<void(const Result<std::vector<Snowflake>> &pageReactorIds)>;
    void fetchPage(Snowflake channelId, const ReactionRef &reaction, std::optional<Snowflake> after, PageFetched onFetched);

    void add(const ReactionRef &reaction, Snowflake userId);
    void remove(const ReactionRef &reaction, Snowflake userId);
    void forgetReactors(Snowflake messageId);
    void forgetReactors(Snowflake messageId, const Discord::Emoji &emoji);

    Platform::Signal<Snowflake> reactorsChanged;

private:
    void onReady();

private:
    struct Key
    {
        Snowflake messageId;
        Snowflake customEmojiId;
        std::string unicodeEmoji;
        bool isBurst;

        bool operator==(const Key &other) const
        {
            return messageId == other.messageId && customEmojiId == other.customEmojiId && unicodeEmoji == other.unicodeEmoji && isBurst == other.isBurst;
        }

        struct Hash {
            size_t operator()(const Key &key) const
            {
                size_t h = std::hash<Snowflake>{}(key.messageId);
                auto combine = [&h](size_t value) { h ^= value + 0x9e3779b9 + (h << 6) + (h >> 2); };
                combine(std::hash<Snowflake>{}(key.customEmojiId));
                combine(std::hash<std::string>{}(key.unicodeEmoji));
                combine(std::hash<bool>{}(key.isBurst));
                return h;
            }
        };
    };

    struct Known
    {
        bool askedApi = false;
        std::vector<Snowflake> reactorIds;
    };

    [[nodiscard]] static Key keyOf(const ReactionRef &reaction);
    void fetch(Snowflake channelId, const ReactionRef &reaction, int limit, std::optional<Snowflake> after, PageFetched onFetched);

    Discord::Client *client;
    UserManager *users;
    std::unordered_map<Key, Known, Key::Hash> knownByReaction;
};

} // namespace Core
} // namespace Styx
