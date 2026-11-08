#include "NotificationText.hpp"

#include <string>
#include <string_view>

namespace Styx::Core {

// Notification centres cut long text themselves, but a short body keeps the toast readable.
constexpr std::size_t MAX_BYTES = 200;

namespace {

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isNameChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool isAsciiLetter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::size_t readDigits(std::string_view s, std::size_t start)
{
    std::size_t pos = start;
    while (pos < s.size() && isDigit(s[pos]))
        pos++;
    return pos - start;
}

} // namespace

std::string notificationText(std::string_view content, int attachments, int stickers,
                             const MentionNames &names)
{
    std::string out;
    std::size_t i = 0;
    const std::size_t n = content.size();

    while (i < n) {
        if (content[i] == '<') {
            if (i + 1 < n && content[i + 1] == '@') {
                std::size_t pos = i + 2;
                bool isRole = false;
                if (pos < n && (content[pos] == '!' || content[pos] == '&')) {
                    if (content[pos] == '&')
                        isRole = true;
                    pos++;
                }
                const std::size_t digitCount = readDigits(content, pos);
                if (digitCount > 0 && pos + digitCount < n && content[pos + digitCount] == '>') {
                    const auto id = content.substr(pos, digitCount);
                    if (isRole) {
                        const std::string name = names.role ? names.role(id) : "";
                        out += '@';
                        out.append(name.empty() ? "deleted-role" : name);
                    } else {
                        const std::string name = names.user ? names.user(id) : "";
                        out += '@';
                        out.append(name.empty() ? "unknown-user" : name);
                    }
                    i = pos + digitCount + 1;
                    continue;
                }
            } else if (i + 1 < n && content[i + 1] == '#') {
                const std::size_t pos = i + 2;
                const std::size_t digitCount = readDigits(content, pos);
                if (digitCount > 0 && pos + digitCount < n && content[pos + digitCount] == '>') {
                    const auto id = content.substr(pos, digitCount);
                    const std::string name = names.channel ? names.channel(id) : "";
                    out += '#';
                    out.append(name.empty() ? "deleted-channel" : name);
                    i = pos + digitCount + 1;
                    continue;
                }
            } else if ((i + 1 < n && content[i + 1] == ':') ||
                       (i + 2 < n && content[i + 1] == 'a' && content[i + 2] == ':')) {
                std::size_t pos = (content[i + 1] == ':') ? (i + 2) : (i + 3);
                const std::size_t nameStart = pos;
                while (pos < n && isNameChar(content[pos]))
                    pos++;
                if (pos > nameStart && pos < n && content[pos] == ':') {
                    const auto name = content.substr(nameStart, pos - nameStart);
                    const std::size_t digitStart = pos + 1;
                    const std::size_t digitCount = readDigits(content, digitStart);
                    if (digitCount > 0 && digitStart + digitCount < n && content[digitStart + digitCount] == '>') {
                        out += ':';
                        out.append(name);
                        out += ':';
                        i = digitStart + digitCount + 1;
                        continue;
                    }
                }
            } else if (i + 2 < n && content[i + 1] == 't' && content[i + 2] == ':') {
                const std::size_t pos = i + 3;
                const std::size_t digitCount = readDigits(content, pos);
                if (digitCount > 0) {
                    const auto digits = content.substr(pos, digitCount);
                    const std::size_t afterDigits = pos + digitCount;
                    if (afterDigits < n && content[afterDigits] == '>') {
                        out.append(digits);
                        i = afterDigits + 1;
                        continue;
                    }
                    if (afterDigits + 2 < n && content[afterDigits] == ':' &&
                        isAsciiLetter(content[afterDigits + 1]) && content[afterDigits + 2] == '>') {
                        out.append(digits);
                        i = afterDigits + 3;
                        continue;
                    }
                }
            }
            out += '<';
            i++;
            continue;
        }

        if (i + 1 < n && content[i] == '|' && content[i + 1] == '|') {
            const auto closePos = content.find("||", i + 2);
            if (closePos != std::string_view::npos) {
                out.append("(spoiler)");
                i = closePos + 2;
            } else {
                out.append("||");
                i += 2;
            }
            continue;
        }

        if (i + 1 < n &&
            ((content[i] == '*' && content[i + 1] == '*') ||
             (content[i] == '_' && content[i + 1] == '_') ||
             (content[i] == '~' && content[i + 1] == '~'))) {
            i += 2;
            continue;
        }
        if (content[i] == '`') {
            i++;
            continue;
        }

        out += content[i];
        i++;
    }

    std::size_t start = 0;
    while (start < out.size() && isWhitespace(out[start]))
        start++;
    std::size_t end = out.size();
    while (end > start && isWhitespace(out[end - 1]))
        end--;
    if (start > 0 || end < out.size())
        out = out.substr(start, end - start);

    if (out.empty()) {
        if (attachments == 1)
            return "Sent an attachment";
        if (attachments > 1)
            return "Sent " + std::to_string(attachments) + " attachments";
        if (stickers > 0)
            return "Sent a sticker";
        return "";
    }

    if (out.size() > MAX_BYTES) {
        std::size_t cut = MAX_BYTES;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            cut--;
        out.resize(cut);
        while (!out.empty() && isWhitespace(out.back()))
            out.pop_back();
        out += "\xE2\x80\xA6";
    }

    return out;
}

} // namespace Styx::Core
