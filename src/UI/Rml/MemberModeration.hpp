#pragma once

#include "Platform/Native.hpp"
#include <string>
#include <memory>
#include <optional>
#include <vector>

#include "Core/Snowflake.hpp"

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {
class Shell;
namespace Rml {
struct MenuItem;
}

struct MemberModerationTarget
{
    Core::Snowflake guildId;
    Core::Snowflake userId;
    std::string username;
    std::vector<Core::Snowflake> roleIds;
    bool bot = false;
    Platform::DateTime timeoutUntil;
    [[nodiscard]] bool isTimedOut() const { return timeoutUntil.valid() && timeoutUntil > Platform::DateTime::now(); }
};

namespace MemberModeration {
class Controller
{
public:
    struct Impl;
    explicit Controller(Shell &shell);
    ~Controller();
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

private:
    std::unique_ptr<Impl> impl;
};

[[nodiscard]] std::optional<MemberModerationTarget> targetFor(Core::ClientInstance *instance, Core::Snowflake guildId, Core::Snowflake userId);
[[nodiscard]] bool canKick(Core::ClientInstance *instance, const MemberModerationTarget &target);
[[nodiscard]] bool canBan(Core::ClientInstance *instance, const MemberModerationTarget &target);
[[nodiscard]] bool canTimeout(Core::ClientInstance *instance, const MemberModerationTarget &target);
[[nodiscard]] bool canChangeNickname(Core::ClientInstance *instance, const MemberModerationTarget &target);
[[nodiscard]] bool canEditRoles(Core::ClientInstance *instance, const MemberModerationTarget &target);
[[nodiscard]] std::string timeoutRemaining(const Platform::DateTime &until);

void kick(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target);
void ban(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target);
void timeout(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target);
void removeTimeout(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target);
void changeNickname(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target);
void addModerationActions(std::vector<Rml::MenuItem> &items, Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target);
} // namespace MemberModeration
} // namespace UI
} // namespace Styx
