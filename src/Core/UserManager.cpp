#include "UserManager.hpp"

#include "Logging.hpp"
#include "RelationshipManager.hpp"
#include "Discord/CdnUrls.hpp"

namespace Styx {
namespace Core {

UserManager::UserManager(Snowflake accountId)
    : userRepo(accountId), memberRepo(accountId)
{
}

std::optional<Discord::User> UserManager::getUser(Snowflake userId)
{
    if (auto *user = userCache.get(userId))
        return *user;

    auto dbUser = userRepo.getUser(userId);
    if (!dbUser.has_value())
        return std::nullopt;

    userCache.put(userId, dbUser.value());
    return dbUser;
}

std::optional<Discord::Member> UserManager::getMember(Snowflake guildId, Snowflake userId)
{
    MemberKey key{ guildId, userId };

    if (auto *member = memberCache.get(key))
        return *member;

    auto dbMember = memberRepo.getMember(guildId, userId);
    auto dbUser = userRepo.getUser(userId);
    if (!dbMember.has_value() || !dbUser.has_value())
        return std::nullopt;

    dbMember->user = dbUser.value();
    memberCache.put(key, dbMember.value());
    return dbMember;
}

std::optional<std::vector<Snowflake>> UserManager::getMemberRoles(Snowflake guildId, Snowflake userId)
{
    if (auto *member = memberCache.get(MemberKey{ guildId, userId }))
        return member->roles.hasValue() ? member->roles.get() : std::vector<Snowflake>{};

    auto dbMember = memberRepo.getMember(guildId, userId);
    if (!dbMember.has_value())
        return std::nullopt;
    return dbMember->roles.hasValue() ? dbMember->roles.get() : std::vector<Snowflake>{};
}

std::vector<Discord::Member> UserManager::getKnownMembers(Snowflake guildId)
{
    return memberRepo.getMembersWithUsers(guildId);
}

void UserManager::setRelationshipManager(RelationshipManager *manager)
{
    relationshipManager = manager;
}

std::string UserManager::getNickname(Snowflake userId, Snowflake guildId)
{
    if (!guildId.isValid())
        return relationshipManager ? relationshipManager->getNickname(userId) : std::string();

    auto member = getMember(guildId, userId);
    return member ? member->nick.valueOr() : std::string();
}

std::string UserManager::getDisplayName(Snowflake userId, Snowflake guildId)
{
    auto user = getUser(userId);
    if (!user)
        return "Unknown User";

    std::string nick = getNickname(userId, guildId);
    return nick.empty() ? user->getDisplayName() : nick;
}

std::string UserManager::getAuthorDisplayName(const Discord::User &author, Snowflake guildId)
{
    Snowflake authorId = author.id.get();
    std::string nick = getNickname(authorId, guildId);
    if (!nick.empty())
        return nick;

    if (!author.bot.valueOr(false)) {
        if (auto cached = getUser(authorId))
            return cached->getDisplayName();
    }
    return author.getDisplayName();
}

std::string UserManager::getAvatarUrl(const Discord::User &user, Snowflake guildId, int size)
{
    Snowflake userId = user.id.get();
    std::string memberHash;
    if (guildId.isValid()) {
        if (auto member = getMember(guildId, userId))
            memberHash = member->avatar.valueOr();
    }
    return Discord::Cdn::effectiveAvatar(guildId, userId, memberHash, user.avatar.get(), size);
}

void UserManager::saveUser(const Discord::User &user)
{
    userCache.put(user.id.get(), user);
    userRepo.saveUser(user);
}

void UserManager::saveUsers(const std::vector<Discord::User> &users)
{
    for (const auto &user : users)
        userCache.put(user.id.get(), user);

    userRepo.saveUsers(users);
}

void UserManager::saveMember(Snowflake guildId, Snowflake userId, const Discord::Member &member)
{
    MemberKey key{ guildId, userId };

    memberCache.put(key, member);
    memberRepo.saveMember(guildId, userId, member);
}

void UserManager::saveMembers(Snowflake guildId, const std::vector<Discord::Member> &members)
{
    for (const auto &member : members) {
        if (!member.user.hasValue() || !member.user->id.hasValue())
            continue;
        Snowflake userId = member.user->id.get();
        memberCache.put(MemberKey{ guildId, userId }, member);
    }
    memberRepo.saveMembers(guildId, members);
}

void UserManager::removeGuildMembers(Snowflake guildId)
{
    memberCache.eraseIf([guildId](const MemberKey &key) { return key.guildId == guildId; });
}

void UserManager::saveMemberWithUser(Snowflake guildId, const Discord::Member &member)
{
    if (member.user.hasValue()) {
        saveUser(member.user.get());
        saveMember(guildId, member.user->id.get(), member);
    }
}

void UserManager::removeMember(Snowflake guildId, Snowflake userId)
{
    memberCache.erase(MemberKey{ guildId, userId });
    memberRepo.deleteMember(guildId, userId);
}

void UserManager::loadNotesFromReady(const std::unordered_map<Snowflake, std::string> &readyNotes)
{
    notes = readyNotes;
    log(LogLevel::Info, LogCore) << "Loaded" << notes.size() << "user notes from READY";
}

void UserManager::setCachedNote(Snowflake userId, const std::string &note)
{
    if (note.empty())
        notes.erase(userId);
    else
        notes.insert_or_assign(userId, note);
    noteChanged.publish(userId);
}

std::optional<std::string> UserManager::getCachedNote(Snowflake userId) const
{
    auto it = notes.find(userId);
    if (it == notes.end())
        return std::nullopt;
    return it->second;
}

} // namespace Core
} // namespace Styx
