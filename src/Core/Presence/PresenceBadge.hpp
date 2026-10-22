#pragma once

#include <QMetaType>

#include "Discord/Enums.hpp"

namespace Styx {
namespace Core {

struct PresenceBadge
{
    Discord::StatusType status = Discord::StatusType::OFFLINE;
    bool mobile = false;
    bool streaming = false;
};

} // namespace Core
} // namespace Styx

Q_DECLARE_METATYPE(Styx::Core::PresenceBadge)
