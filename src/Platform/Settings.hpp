#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "Signal.hpp"

namespace Styx::Platform {

class Settings {
public:
    static Settings &instance();

    Settings();
    explicit Settings(std::filesystem::path filePath);
    ~Settings();

    Settings(const Settings &) = delete;
    Settings &operator=(const Settings &) = delete;

    // Load settings from disk (default: Paths::configDir() / "settings.json")
    bool load();
    bool loadFromFile(const std::filesystem::path &path);

    // Atomically persist settings to disk (write to .tmp then replace)
    bool save();
    bool saveToFile(const std::filesystem::path &path);

    // Typed getters
    bool getBool(std::string_view key, bool defaultValue = false) const;
    int getInt(std::string_view key, int defaultValue = 0) const;
    int64_t getInt64(std::string_view key, int64_t defaultValue = 0) const;
    double getDouble(std::string_view key, double defaultValue = 0.0) const;
    std::string getString(std::string_view key, std::string_view defaultValue = "") const;
    nlohmann::json getJson(std::string_view key, const nlohmann::json &defaultValue = {}) const;

    // Typed setters (triggers change notification if value changed)
    void setBool(std::string_view key, bool value);
    void setInt(std::string_view key, int value);
    void setInt64(std::string_view key, int64_t value);
    void setDouble(std::string_view key, double value);
    void setString(std::string_view key, std::string_view value);
    void setJson(std::string_view key, const nlohmann::json &value);

    template <typename T>
    void set(std::string_view key, const T &value)
    {
        setJson(key, nlohmann::json(value));
    }

    template <typename T>
    T get(std::string_view key, const T &defaultValue) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string k(key);
        if (m_data.is_object() && m_data.contains(k)) {
            try {
                return m_data[k].get<T>();
            } catch (...) {
            }
        }
        return defaultValue;
    }

    bool contains(std::string_view key) const;
    void remove(std::string_view key);
    void clear();

    std::filesystem::path filePath() const;
    void setFilePath(std::filesystem::path path);

    // Emitted whenever a setting changes: (key, newValue)
    Signal<std::string, nlohmann::json> changed;

#ifdef _WIN32
    // One-time Win32 registry importer that reads all QSettings keys from
    // HKCU\Software\larrythemobster\Styx via Win32 registry API (no Qt).
    static std::unordered_map<std::string, nlohmann::json> importFromRegistry(
        const std::wstring &rootKey = L"Software\\larrythemobster\\Styx");

    // Reads registry keys and applies them to this Settings instance
    size_t importAndApplyRegistry(bool saveAfter = true);
#endif

private:
    mutable std::mutex m_mutex;
    std::filesystem::path m_path;
    nlohmann::json m_data;
};

} // namespace Styx::Platform
