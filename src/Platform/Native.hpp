#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Platform {

struct Size {
    int width = 0;
    int height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
    auto operator<=>(const Size &) const = default;
};
struct Point {
    float x = 0;
    float y = 0;
    auto operator<=>(const Point &) const = default;
};
struct Rect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
    bool contains(Point point) const { return point.x >= x && point.y >= y && point.x < x + width && point.y < y + height; }
    auto operator<=>(const Rect &) const = default;
};
Size fitWithin(Size source, Size bounds);
float primaryDevicePixelRatio();

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool valid = false;
    static Color rgb(uint32_t value);
    static Color rgba(uint32_t value);
    static Color parse(std::string_view text);
    std::string css() const;
    uint32_t argb() const;
    auto operator<=>(const Color &) const = default;
};

class DateTime {
public:
    using Clock = std::chrono::system_clock;
    using TimePoint = std::chrono::time_point<Clock, std::chrono::milliseconds>;
    DateTime() = default;
    explicit DateTime(TimePoint value) : value_(value) {}
    static DateTime now();
    static DateTime fromUnixMillis(int64_t milliseconds);
    static DateTime parse(std::string_view iso8601);
    bool valid() const { return value_.has_value(); }
    int64_t unixMillis() const;
    std::string iso8601() const;
    std::string format(std::string_view pattern) const;
    DateTime addSeconds(int64_t seconds) const;
    DateTime addMillis(int64_t milliseconds) const;
    int64_t secondsUntil(const DateTime &other) const;
    int64_t millisUntil(const DateTime &other) const;
    bool sameDate(const DateTime &other) const;
    auto operator<=>(const DateTime &) const = default;
private:
    std::optional<TimePoint> value_;
};

namespace Resources {
std::filesystem::path path(std::string_view relative);
std::string read(std::string_view relative);
std::string readFile(const std::filesystem::path &path);
bool writeFile(const std::filesystem::path &path, std::string_view bytes);
}

std::string htmlEscape(std::string_view text);
std::string urlEncode(std::string_view text);
std::string urlDecode(std::string_view text);
std::string urlHost(std::string_view url);
std::string urlPath(std::string_view url);
std::string fileUrl(const std::filesystem::path &path);
std::filesystem::path localFilePath(std::string_view url);
std::string mimeType(const std::filesystem::path &path);
std::string randomUuid();
std::vector<char32_t> codepoints(std::string_view utf8);
std::string fromCodepoints(const std::vector<char32_t> &values);
std::string unicodeLower(std::string_view utf8);
std::string unicodeUpper(std::string_view utf8);

}
