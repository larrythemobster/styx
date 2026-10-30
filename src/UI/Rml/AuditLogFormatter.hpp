#pragma once

#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Platform/Trackable.hpp"
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Styx::Core { class ClientInstance; }
namespace Styx::UI {
class AuditLogFormatter {
public:
    enum class Category { Other, Create, Update, Delete };
    struct Line { std::string html; std::vector<std::string> items; };
    struct Row {
        Discord::AuditLogAction action;
        Core::Snowflake userId;
        std::string titleHtml;
        Platform::DateTime start, end;
        Category category = Category::Other;
        std::vector<Line> lines;
    };
    AuditLogFormatter(Core::ClientInstance *instance, Core::Snowflake guildId);
    std::vector<Row> addPage(const Discord::AuditLog &page);
    std::optional<Discord::User> user(Core::Snowflake userId) const;
    static std::string timeText(const Platform::DateTime &start, const Platform::DateTime &end);
    static std::vector<std::pair<Discord::AuditLogAction, std::string>> actionFilters();
private:
    enum class Target;
    struct Change;
    struct Entry;
    static Target targetOf(Discord::AuditLogAction action);
    static Category categoryOf(Discord::AuditLogAction action);
    static bool neverMerged(Discord::AuditLogAction action);
    void rememberDeletedName(const Entry &entry);
    Row format(const std::vector<Entry> &merged);
    std::string titleFor(const Entry &entry, const std::string &target, const std::string &count, const std::string &channel, const std::string &subtarget) const;
    std::string targetName(const Entry &entry) const;
    std::string authorName(const Entry &entry) const;
    std::vector<Change> transform(const Entry &entry) const;
    std::optional<Line> describe(const Entry &entry, const Change &change) const;
    std::string channelName(Core::Snowflake channelId) const;
    std::string roleName(Core::Snowflake roleId) const;
    std::string userTag(Core::Snowflake userId) const;
    Platform::Guard<Core::ClientInstance> instance;
    Core::Snowflake guildId;
    std::unordered_map<Core::Snowflake, Discord::User> users;
    std::unordered_map<Core::Snowflake, nlohmann::json> integrations, webhooks, scheduledEvents, autoModerationRules, threads, applicationCommands;
    std::map<std::pair<int, Core::Snowflake>, std::string> deletedNames;
};
}
