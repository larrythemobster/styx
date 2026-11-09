#pragma once

#include "Core/JsonUtils.hpp"
#include <string>

namespace Styx {
namespace Discord {

struct CurrentUserEdit : Core::JsonUtils::JsonObject
{
    Field<std::string, true, true> globalName;
    Field<std::string, true, true> avatar;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "global_name", globalName);
        insert(obj, "avatar", avatar);
        return obj;
    }

    bool empty() const
    {
        return globalName.isUndefined() && avatar.isUndefined();
    }
};

struct UserProfileEdit : Core::JsonUtils::JsonObject
{
    Field<std::string, true, true> pronouns;
    Field<std::string, true, true> bio;
    Field<std::string, true, true> banner;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "pronouns", pronouns);
        insert(obj, "bio", bio);
        insert(obj, "banner", banner);
        return obj;
    }

    bool empty() const
    {
        return pronouns.isUndefined() && bio.isUndefined() && banner.isUndefined();
    }
};

} // namespace Discord
} // namespace Styx
