#include "Native.hpp"
#include "Paths.hpp"
#include "Utf8.hpp"

#include <curl/curl.h>
#include <openssl/rand.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace Styx::Platform {
namespace {
std::string pathUtf8(const std::filesystem::path &path)
{
    auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
std::filesystem::path utf8Path(std::string_view value)
{
    return std::filesystem::u8path(value.begin(), value.end());
}
int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::tm localTm(std::time_t time)
{
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &time);
#else
    localtime_r(&time, &result);
#endif
    return result;
}
std::tm utcTm(std::time_t time)
{
    std::tm result{};
#ifdef _WIN32
    gmtime_s(&result, &time);
#else
    gmtime_r(&time, &result);
#endif
    return result;
}
bool number(std::string_view text, size_t pos, size_t count, int &value)
{
    if (pos + count > text.size()) return false;
    value = 0;
    for (size_t i = pos; i < pos + count; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + text[i] - '0';
    }
    return true;
}
std::string urlPart(std::string_view text, CURLUPart part, unsigned flags = 0)
{
    using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
    Url url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, std::string(text).c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK) return {};
    char *raw = nullptr;
    if (curl_url_get(url.get(), part, &raw, flags) != CURLUE_OK) return {};
    std::unique_ptr<char, decltype(&curl_free)> result(raw, curl_free);
    return result.get();
}
std::string encode(std::string_view text, bool path)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || (path && (c == '/' || c == ':'))) out += static_cast<char>(c);
        else { out += '%'; out += digits[c >> 4]; out += digits[c & 15]; }
    }
    return out;
}
}

Size fitWithin(Size source, Size bounds)
{
    if (source.empty() || bounds.empty()) return {};
    const double scale = std::min(static_cast<double>(bounds.width) / source.width, static_cast<double>(bounds.height) / source.height);
    return {std::max(1, static_cast<int>(std::round(source.width * scale))), std::max(1, static_cast<int>(std::round(source.height * scale)))};
}
Color Color::rgb(uint32_t value) { return {static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value), 255, true}; }
Color Color::rgba(uint32_t value) { return {static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 24), true}; }
Color Color::parse(std::string_view text)
{
    text = trim(text);
    if (text.size() > 1 && text[0] == '#') {
        auto s = text.substr(1);
        if (s.size() != 3 && s.size() != 4 && s.size() != 6 && s.size() != 8) return {};
        std::array<int, 4> v{0, 0, 0, 255};
        const bool shortForm = s.size() <= 4;
        const size_t count = shortForm ? s.size() : s.size() / 2;
        for (size_t i = 0; i < count; ++i) {
            int n = hex(s[shortForm ? i : i * 2]);
            if (n < 0) return {};
            if (shortForm) v[i] = n * 17;
            else { int low = hex(s[i * 2 + 1]); if (low < 0) return {}; v[i] = n * 16 + low; }
        }
        return {static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]), static_cast<uint8_t>(v[2]), static_cast<uint8_t>(v[3]), true};
    }
    const bool alpha = text.starts_with("rgba(");
    if (!(alpha || text.starts_with("rgb(")) || text.back() != ')') return {};
    auto components = split(text.substr(alpha ? 5 : 4, text.size() - (alpha ? 6 : 5)), ',');
    if (components.size() != (alpha ? 4u : 3u)) return {};
    std::array<uint8_t, 4> values{0, 0, 0, 255};
    for (size_t i = 0; i < components.size(); ++i) {
        auto part = trim(components[i]);
        bool percent = part.ends_with('%');
        if (percent) part.remove_suffix(1);
        std::string s(part);
        char *end = nullptr;
        double value = std::strtod(s.c_str(), &end);
        if (s.empty() || end != s.c_str() + s.size() || !std::isfinite(value)) return {};
        if (percent) value *= 2.55;
        else if (i == 3) value *= 255;
        values[i] = static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
    }
    return {values[0], values[1], values[2], values[3], true};
}
std::string Color::css() const
{
    if (!valid) return {};
    char result[10];
    if (a == 255) std::snprintf(result, sizeof(result), "#%02x%02x%02x", r, g, b);
    else std::snprintf(result, sizeof(result), "#%02x%02x%02x%02x", r, g, b, a);
    return result;
}
uint32_t Color::argb() const { return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b; }

DateTime DateTime::now() { return DateTime(std::chrono::time_point_cast<std::chrono::milliseconds>(Clock::now())); }
DateTime DateTime::fromUnixMillis(int64_t ms) { return DateTime(TimePoint(std::chrono::milliseconds(ms))); }
DateTime DateTime::parse(std::string_view s)
{
    using namespace std::chrono;
    int y, m, d, hour = 0, minute = 0, second = 0;
    if (s.size() < 10 || s[4] != '-' || s[7] != '-' || !number(s,0,4,y) || !number(s,5,2,m) || !number(s,8,2,d)) return {};
    year_month_day date{year{y}, month{static_cast<unsigned>(m)}, day{static_cast<unsigned>(d)}};
    if (!date.ok()) return {};
    size_t pos = 10;
    int ms = 0, offset = 0;
    bool zone = false;
    if (pos < s.size() && (s[pos] == 'T' || s[pos] == 't' || s[pos] == ' ')) {
        ++pos;
        if (!number(s,pos,2,hour) || pos + 5 > s.size() || s[pos+2] != ':' || !number(s,pos+3,2,minute)) return {};
        pos += 5;
        if (pos < s.size() && s[pos] == ':') { ++pos; if (!number(s,pos,2,second)) return {}; pos += 2; }
        if (hour > 23 || minute > 59 || second > 59) return {};
        if (pos < s.size() && (s[pos] == '.' || s[pos] == ',')) {
            ++pos; size_t start = pos; int scale = 100;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') { if (scale) { ms += (s[pos]-'0') * scale; scale /= 10; } ++pos; }
            if (pos == start) return {};
        }
        if (pos < s.size() && (s[pos] == 'Z' || s[pos] == 'z')) { zone = true; ++pos; }
        else if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
            int sign = s[pos++] == '+' ? 1 : -1, oh, om = 0;
            if (!number(s,pos,2,oh)) return {}; pos += 2;
            if (pos < s.size() && s[pos] == ':') { ++pos; if (!number(s,pos,2,om)) return {}; pos += 2; }
            else if (pos + 2 <= s.size()) { if (!number(s,pos,2,om)) return {}; pos += 2; }
            if (oh > 23 || om > 59) return {};
            offset = sign * (oh * 60 + om); zone = true;
        }
    }
    if (pos != s.size()) return {};
    if (!zone) {
        std::tm local{}; local.tm_year = y - 1900; local.tm_mon = m - 1; local.tm_mday = d; local.tm_hour = hour; local.tm_min = minute; local.tm_sec = second; local.tm_isdst = -1;
        auto time = std::mktime(&local);
        if (time == static_cast<std::time_t>(-1) && (local.tm_year != y - 1900 || local.tm_mon != m - 1 || local.tm_mday != d || local.tm_hour != hour || local.tm_min != minute || local.tm_sec != second)) return {};
        return fromUnixMillis(static_cast<int64_t>(time) * 1000 + ms);
    }
    auto time = sys_days{date} + hours{hour} + minutes{minute - offset} + seconds{second} + milliseconds{ms};
    return DateTime(time_point_cast<milliseconds>(time));
}
int64_t DateTime::unixMillis() const { return value_ ? value_->time_since_epoch().count() : 0; }
std::string DateTime::iso8601() const
{
    if (!value_) return {};
    auto secs = std::chrono::floor<std::chrono::seconds>(*value_);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*value_ - secs).count();
    auto tm = utcTm(Clock::to_time_t(secs));
    char text[40]; std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return text;
}
std::string DateTime::format(std::string_view pattern) const
{
    if (!value_ || pattern.empty()) return {};
    auto tm = localTm(Clock::to_time_t(std::chrono::floor<std::chrono::seconds>(*value_)));
    std::string fmt(pattern), output(256, '\0');
    while (output.size() <= 65536) {
        size_t size = std::strftime(output.data(), output.size(), fmt.c_str(), &tm);
        if (size) { output.resize(size); return output; }
        output.resize(output.size() * 2);
    }
    return {};
}
DateTime DateTime::addSeconds(int64_t seconds) const { return value_ ? DateTime(*value_ + std::chrono::seconds(seconds)) : DateTime{}; }
DateTime DateTime::addMillis(int64_t ms) const { return value_ ? DateTime(*value_ + std::chrono::milliseconds(ms)) : DateTime{}; }
int64_t DateTime::secondsUntil(const DateTime &other) const { return value_ && other.value_ ? std::chrono::duration_cast<std::chrono::seconds>(*other.value_ - *value_).count() : 0; }
int64_t DateTime::millisUntil(const DateTime &other) const { return value_ && other.value_ ? (*other.value_ - *value_).count() : 0; }
bool DateTime::sameDate(const DateTime &other) const
{
    if (!value_ || !other.value_) return false;
    auto a = localTm(Clock::to_time_t(std::chrono::floor<std::chrono::seconds>(*value_)));
    auto b = localTm(Clock::to_time_t(std::chrono::floor<std::chrono::seconds>(*other.value_)));
    return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
}

namespace Resources {
std::filesystem::path path(std::string_view relative)
{
    auto name = utf8Path(relative);
    if (relative.empty() || relative.starts_with(":/") || name.is_absolute() || name.has_root_name()) throw std::invalid_argument("Invalid resource path: " + std::string(relative));
    for (const auto &part : name) if (part == "..") throw std::invalid_argument("Invalid resource path: " + std::string(relative));
    auto installed = Paths::executablePath().parent_path() / "resources" / name;
    if (std::filesystem::exists(installed)) return installed;
#ifdef STYX_RESOURCE_DIR
    auto source = utf8Path(STYX_RESOURCE_DIR) / name;
    if (std::filesystem::exists(source)) return source;
#endif
    return installed;
}
std::string read(std::string_view relative) { return readFile(path(relative)); }
std::string readFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open file: " + pathUtf8(path));
    std::ostringstream stream; stream << file.rdbuf();
    if (file.bad() || stream.bad()) throw std::runtime_error("Cannot read file: " + pathUtf8(path));
    return stream.str();
}
bool writeFile(const std::filesystem::path &path, std::string_view bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close(); return !file.fail();
}
}
std::string htmlEscape(std::string_view text)
{
    std::string out;
    for (char c : text) switch (c) { case '&': out += "&amp;"; break; case '<': out += "&lt;"; break; case '>': out += "&gt;"; break; case '"': out += "&quot;"; break; case '\'': out += "&#39;"; break; default: out += c; }
    return out;
}
std::string urlEncode(std::string_view text) { return encode(text, false); }
std::string urlDecode(std::string_view text)
{
    std::string out; out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) { int a = hex(text[i+1]), b = hex(text[i+2]); if (a >= 0 && b >= 0) { out += static_cast<char>(a * 16 + b); i += 2; continue; } }
        out += text[i];
    }
    return out;
}
std::string urlHost(std::string_view url) { return urlPart(url, CURLUPART_HOST, CURLU_URLDECODE); }
std::string urlPath(std::string_view url) { return urlPart(url, CURLUPART_PATH, CURLU_URLDECODE); }
std::string fileUrl(const std::filesystem::path &path)
{
    auto value = pathUtf8(std::filesystem::absolute(path));
    if (value.starts_with("//")) return "file:" + encode(value, true);
    if (!value.starts_with('/')) value.insert(value.begin(), '/');
    return "file://" + encode(value, true);
}
std::filesystem::path localFilePath(std::string_view url)
{
    if (!equalsIgnoreCaseAscii(url.substr(0, std::min<size_t>(5, url.size())), "file:")) return {};
    url.remove_prefix(5);
    auto end = url.find_first_of("?#");
    url = url.substr(0, end);
    std::string host;
    if (url.starts_with("//")) {
        url.remove_prefix(2);
        auto slash = url.find('/');
        if (slash == std::string_view::npos) return {};
        host = urlDecode(url.substr(0, slash));
        url.remove_prefix(slash);
    }
    auto path = urlDecode(url);
    if (path.empty()) return {};
    if (!host.empty() && !equalsIgnoreCaseAscii(host, "localhost")) path = "//" + host + path;
#ifdef _WIN32
    else if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path.erase(0, 1);
#endif
    return utf8Path(path);
}
std::string mimeType(const std::filesystem::path &path)
{
    auto ext = toLowerAscii(pathUtf8(path.extension()));
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".bmp") return "image/bmp";
    if (ext == ".avif") return "image/avif";
    if (ext == ".mp4") return "video/mp4";
    if (ext == ".webm") return "video/webm";
    if (ext == ".mov") return "video/quicktime";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".ogg" || ext == ".opus") return "audio/ogg";
    if (ext == ".wav") return "audio/wav";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".txt") return "text/plain";
    if (ext == ".json") return "application/json";
    return "application/octet-stream";
}
std::string randomUuid()
{
    std::array<unsigned char, 16> data{};
    if (RAND_bytes(data.data(), static_cast<int>(data.size())) != 1) throw std::runtime_error("Unable to generate UUID");
    data[6] = (data[6] & 0x0f) | 0x40; data[8] = (data[8] & 0x3f) | 0x80;
    constexpr char digits[] = "0123456789abcdef";
    std::string out; out.reserve(36);
    for (size_t i = 0; i < data.size(); ++i) { if (i == 4 || i == 6 || i == 8 || i == 10) out += '-'; out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
    return out;
}
std::vector<char32_t> codepoints(std::string_view utf8)
{
    std::vector<char32_t> result;
    size_t i = 0;
    while (i < utf8.size()) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            result.push_back(c);
            i += 1;
        } else if ((c >> 5) == 0x06 && i + 1 < utf8.size()) {
            result.push_back(((c & 0x1F) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F));
            i += 2;
        } else if ((c >> 4) == 0x0E && i + 2 < utf8.size()) {
            result.push_back(((c & 0x0F) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6) |
                             (static_cast<unsigned char>(utf8[i + 2]) & 0x3F));
            i += 3;
        } else if ((c >> 3) == 0x1E && i + 3 < utf8.size()) {
            result.push_back(((c & 0x07) << 18) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 12) |
                             ((static_cast<unsigned char>(utf8[i + 2]) & 0x3F) << 6) |
                             (static_cast<unsigned char>(utf8[i + 3]) & 0x3F));
            i += 4;
        } else {
            result.push_back(0xFFFD);
            i += 1;
        }
    }
    return result;
}
std::string fromCodepoints(const std::vector<char32_t> &values)
{
    std::string result;
    for (char32_t cp : values) {
        if (cp < 0x80) {
            result.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x110000) {
            result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            result.append("\xEF\xBF\xBD");
        }
    }
    return result;
}
std::string unicodeLower(std::string_view text)
{
#ifdef _WIN32
    std::wstring wide = toUtf16(text);
    CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    return fromUtf16(wide);
#else
    return toLowerAscii(text);
#endif
}
std::string unicodeUpper(std::string_view text)
{
#ifdef _WIN32
    std::wstring wide = toUtf16(text);
    CharUpperBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    return fromUtf16(wide);
#else
    return toUpperAscii(text);
#endif
}
