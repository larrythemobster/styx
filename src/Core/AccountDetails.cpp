#include "Core/AccountDetails.hpp"

#include <cstddef>

namespace Styx::Core {
namespace {
// UTF-8 continuation bytes don't start a character.
bool startsCharacter(char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }

std::size_t characterCount(std::string_view text)
{
    std::size_t count = 0;
    for (const char c : text) count += startsCharacter(c);
    return count;
}
}

std::string maskEmail(std::string_view email)
{
    const auto at = email.rfind('@');
    if (at == std::string_view::npos)
        return std::string(characterCount(email), '*');
    return std::string(characterCount(email.substr(0, at)), '*') + std::string(email.substr(at));
}

std::string maskPhone(std::string_view phone)
{
    const std::size_t total = characterCount(phone);
    if (total <= 4)
        return std::string(total, '*');
    // Keep the last four characters, wherever their bytes start.
    std::size_t kept = 0, start = phone.size();
    while (start > 0 && kept < 4) {
        --start;
        kept += startsCharacter(phone[start]);
    }
    return std::string(total - 4, '*') + std::string(phone.substr(start));
}

} // namespace Styx::Core
