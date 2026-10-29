#pragma once

#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "Platform/Signal.hpp"

#include "Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Storage/UserRepository.hpp"
#include "Storage/MemberRepository.hpp"

namespace Styx {
namespace Core {

class RelationshipManager;

struct MemberKey
{
    Snowflake guildId;
    Snowflake userId;

    auto operator<=>(const MemberKey &) const = default;
};

template <typename Key, typename Value>
class ManagerCache
{
public:
    const Value *get(const Key &key)
    {
        auto it = index.find(key);
        if (it == index.end())
            return nullptr;
        entries.splice(entries.begin(), entries, it->second);
        return &it->second->second;
    }
    void put(const Key &key, const Value &value)
    {
        erase(key);
        entries.emplace_front(key, value);
        index.emplace(key, entries.begin());
        if (entries.size() > 100) {
            index.erase(entries.back().first);
            entries.pop_back();
        }
    }
    void erase(const Key &key)
    {
        auto it = index.find(key);
        if (it == index.end())
            return;
        entries.erase(it->second);
        index.erase(it);
    }
    template <typename Predicate> void eraseIf(Predicate predicate)
    {
        for (auto it = entries.begin(); it != entries.end();) {
            if (predicate(it->first)) {
                index.erase(it->first);
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }
private:
    std::list<std::pair<Key, Value>> entries;
    std::map<Key, typename std::list<std::pair<Key, Value>>::iterator> index;
};

class UserManager
{
public:
    explicit UserManager(Snowflake accountId);

    [[nodiscard]] std::optional<Discord::User> getUser(Snowflake userId);
    [[nodiscard]] std::optional<Discord::Member> getMember(Snowflake guildId, Snowflake userId);

    [[nodiscard]] std::optional<std::vector<Snowflake>> getMemberRoles(Snowflake guildId, Snowflake userId);
    [[nodiscard]] std::vector<Discord::Member> getKnownMembers(Snowflake guildId);

    void setRelationshipManager(RelationshipManager *manager);

    [[nodiscard]] std::string getNickname(Snowflake userId, Snowflake guildId);
    [[nodiscard]] std::string getDisplayName(Snowflake userId, Snowflake guildId = Snowflake::Invalid);
    [[nodiscard]] std::string getAuthorDisplayName(const Discord::User &author, Snowflake guildId);
    [[nodiscard]] std::string getAvatarUrl(const Discord::User &user, Snowflake guildId, int size);

    void saveUser(const Discord::User &user);
    void saveUsers(const std::vector<Discord::User> &users);
    void saveMember(Snowflake guildId, Snowflake userId, const Discord::Member &member);
    void saveMembers(Snowflake guildId, const std::vector<Discord::Member> &members);
    void removeGuildMembers(Snowflake guildId);

    void saveMemberWithUser(Snowflake guildId, const Discord::Member &member);
    void removeMember(Snowflake guildId, Snowflake userId);

    void loadNotesFromReady(const std::unordered_map<Snowflake, std::string> &notes);
    void setCachedNote(Snowflake userId, const std::string &note);
    [[nodiscard]] std::optional<std::string> getCachedNote(Snowflake userId) const;

    Platform::Signal<Snowflake> noteChanged;

private:
    ManagerCache<Snowflake, Discord::User> userCache;
    ManagerCache<MemberKey, Discord::Member> memberCache;

    std::unordered_map<Snowflake, std::string> notes;

    RelationshipManager *relationshipManager = nullptr;

    Storage::UserRepository userRepo;
    Storage::MemberRepository memberRepo;
};

} // namespace Core
} // namespace Styx
