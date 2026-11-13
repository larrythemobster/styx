#pragma once

#include <string>
#include <string_view>

namespace Styx::Core {

// Hides the part of an email address before the domain, one asterisk per character,
// as the account settings page shows it until revealed.
[[nodiscard]] std::string maskEmail(std::string_view email);

// Hides all but the last four characters of a phone number, one asterisk per character.
[[nodiscard]] std::string maskPhone(std::string_view phone);

} // namespace Styx::Core
