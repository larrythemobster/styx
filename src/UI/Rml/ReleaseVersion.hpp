#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::UI::Rml {
inline constexpr std::string_view currentVersion = "0.1.0-alpha.1";

struct ReleaseVersion {
    std::array<std::string, 3> core;
    std::vector<std::string> prerelease;
};

inline bool versionDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}
inline std::optional<ReleaseVersion> parseReleaseVersion(std::string_view s) {
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.remove_prefix(1);
    auto validIdentifiers = [](std::string_view part, bool pre) {
        if (part.empty()) return false;
        while (true) {
            auto dot = part.find('.'); auto id = part.substr(0, dot);
            if (id.empty()) return false;
            for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-')) return false;
            if (pre && versionDigits(id) && id.size() > 1 && id.front() == '0') return false;
            if (dot == part.npos) break;
            part.remove_prefix(dot + 1);
        }
        return true;
    };
    auto plus = s.find('+');
    if (plus != s.npos) { if (!validIdentifiers(s.substr(plus + 1), false)) return std::nullopt; s = s.substr(0, plus); }
    auto dash = s.find('-'); auto pre = dash == s.npos ? std::string_view{} : s.substr(dash + 1);
    if (dash != s.npos && !validIdentifiers(pre, true)) return std::nullopt;
    auto core = s.substr(0, dash); ReleaseVersion result;
    for (size_t i = 0; i < 3; ++i) {
        auto dot = core.find('.'); auto part = core.substr(0, dot);
        if (!versionDigits(part) || (part.size() > 1 && part.front() == '0') || (i < 2 && dot == core.npos) || (i == 2 && dot != core.npos)) return std::nullopt;
        result.core[i] = part;
        if (dot != core.npos) core.remove_prefix(dot + 1);
    }
    while (!pre.empty()) { auto dot = pre.find('.'); result.prerelease.emplace_back(pre.substr(0, dot)); if (dot == pre.npos) break; pre.remove_prefix(dot + 1); }
    return result;
}
inline int compareVersionNumbers(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return a == b ? 0 : (a < b ? -1 : 1);
}
inline int compareReleaseVersions(const ReleaseVersion &a, const ReleaseVersion &b) {
    for (size_t i = 0; i < 3; ++i) if (int c = compareVersionNumbers(a.core[i], b.core[i])) return c;
    if (a.prerelease.empty() != b.prerelease.empty()) return a.prerelease.empty() ? 1 : -1;
    for (size_t i = 0; i < a.prerelease.size() && i < b.prerelease.size(); ++i) {
        const auto &x = a.prerelease[i]; const auto &y = b.prerelease[i];
        bool xn = versionDigits(x), yn = versionDigits(y);
        if (xn != yn) return xn ? -1 : 1;
        int c = xn ? compareVersionNumbers(x, y) : (x == y ? 0 : (x < y ? -1 : 1));
        if (c) return c;
    }
    return a.prerelease.size() == b.prerelease.size() ? 0 : (a.prerelease.size() < b.prerelease.size() ? -1 : 1);
}
}
