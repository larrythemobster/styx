#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

inline constexpr std::string_view LogCore = "styx.core";
inline constexpr std::string_view LogNetwork = "styx.network";
inline constexpr std::string_view LogDiscord = "styx.discord";
inline constexpr std::string_view LogDB = "styx.db";
inline constexpr std::string_view LogUI = "styx.ui";
inline constexpr std::string_view LogProto = "styx.proto";
inline constexpr std::string_view LogDave = "styx.dave";
inline constexpr std::string_view LogMiniaudio = "styx.miniaudio";
inline constexpr std::string_view LogVoice = "styx.voice";
inline constexpr std::string_view LogVideo = "styx.video";
inline constexpr std::string_view LogStream = "styx.stream";

namespace Styx::Core {

enum class LogLevel { Debug, Info, Warning, Error };

class Logger {
public:
    static void init();
    static void cleanup();
    static void write(LogLevel level, std::string_view category, std::string message);
};

class LogLine {
public:
    LogLine(LogLevel level, std::string_view category) : level_(level), category_(category) {}
    ~LogLine() noexcept;
    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;
    LogLine(LogLine &&) = delete;
    template<class T> LogLine &operator<<(const T &value) {
        if (!first_) stream_ << ' ';
        first_ = false;
        if constexpr (std::is_enum_v<T>) stream_ << static_cast<std::underlying_type_t<T>>(value);
        else stream_ << value;
        return *this;
    }
private:
    LogLevel level_;
    std::string_view category_;
    std::ostringstream stream_;
    bool first_ = true;
};

inline LogLine log(LogLevel level, std::string_view category) { return LogLine(level, category); }

}
