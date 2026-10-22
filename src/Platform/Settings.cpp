#include "Settings.hpp"
#include "Paths.hpp"
#include "Utf8.hpp"

#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Styx::Platform {

Settings &Settings::instance()
{
    static Settings s_instance;
    return s_instance;
}

Settings::Settings()
    : Settings(Paths::configDir() / "settings.json")
{
}

Settings::Settings(std::filesystem::path filePath)
    : m_path(std::move(filePath))
    , m_data(nlohmann::json::object())
{
    load();
}

Settings::~Settings() = default;

std::filesystem::path Settings::filePath() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_path;
}

void Settings::setFilePath(std::filesystem::path path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_path = std::move(path);
}

bool Settings::load()
{
    std::filesystem::path p;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        p = m_path;
    }
    return loadFromFile(p);
}

bool Settings::loadFromFile(const std::filesystem::path &path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_data = nlohmann::json::object();
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    try {
        nlohmann::json j;
        file >> j;
        if (!j.is_object()) {
            j = nlohmann::json::object();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_data = std::move(j);
        return true;
    } catch (...) {
        return false;
    }
}

bool Settings::save()
{
    std::filesystem::path p;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        p = m_path;
    }
    return saveToFile(p);
}

bool Settings::saveToFile(const std::filesystem::path &path)
{
    std::string content;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        content = m_data.dump(2);
    }

    Paths::ensureDirExists(path.parent_path());

    std::filesystem::path tempPath = path;
    tempPath += ".tmp";

    {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file.good()) {
            file.close();
            std::error_code ec;
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }

#ifdef _WIN32
    std::wstring tempW = tempPath.wstring();
    std::wstring destW = path.wstring();
    if (!MoveFileExW(tempW.c_str(), destW.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    return true;
#else
    std::error_code ec;
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    return true;
#endif
}

bool Settings::getBool(std::string_view key, bool defaultValue) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string k(key);
    if (m_data.is_object() && m_data.contains(k)) {
        const auto &val = m_data[k];
        if (val.is_boolean()) {
            return val.get<bool>();
        }
        if (val.is_number()) {
            return val.get<int>() != 0;
        }
        if (val.is_string()) {
            std::string s = val.get<std::string>();
            return (s == "true" || s == "1" || s == "yes");
        }
    }
    return defaultValue;
}

int Settings::getInt(std::string_view key, int defaultValue) const
{
    return static_cast<int>(getInt64(key, defaultValue));
}

int64_t Settings::getInt64(std::string_view key, int64_t defaultValue) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string k(key);
    if (m_data.is_object() && m_data.contains(k)) {
        const auto &val = m_data[k];
        if (val.is_number()) {
            return val.get<int64_t>();
        }
        if (val.is_string()) {
            try {
                return std::stoll(val.get<std::string>());
            } catch (...) {
            }
        }
        if (val.is_boolean()) {
            return val.get<bool>() ? 1 : 0;
        }
    }
    return defaultValue;
}

double Settings::getDouble(std::string_view key, double defaultValue) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string k(key);
    if (m_data.is_object() && m_data.contains(k)) {
        const auto &val = m_data[k];
        if (val.is_number()) {
            return val.get<double>();
        }
        if (val.is_string()) {
            try {
                return std::stod(val.get<std::string>());
            } catch (...) {
            }
        }
    }
    return defaultValue;
}

std::string Settings::getString(std::string_view key, std::string_view defaultValue) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string k(key);
    if (m_data.is_object() && m_data.contains(k)) {
        const auto &val = m_data[k];
        if (val.is_string()) {
            return val.get<std::string>();
        }
        if (val.is_boolean()) {
            return val.get<bool>() ? "true" : "false";
        }
        if (val.is_number()) {
            return val.dump();
        }
    }
    return std::string(defaultValue);
}

nlohmann::json Settings::getJson(std::string_view key, const nlohmann::json &defaultValue) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string k(key);
    if (m_data.is_object() && m_data.contains(k)) {
        return m_data[k];
    }
    return defaultValue;
}

void Settings::setBool(std::string_view key, bool value)
{
    setJson(key, nlohmann::json(value));
}

void Settings::setInt(std::string_view key, int value)
{
    setJson(key, nlohmann::json(value));
}

void Settings::setInt64(std::string_view key, int64_t value)
{
    setJson(key, nlohmann::json(value));
}

void Settings::setDouble(std::string_view key, double value)
{
    setJson(key, nlohmann::json(value));
}

void Settings::setString(std::string_view key, std::string_view value)
{
    setJson(key, nlohmann::json(std::string(value)));
}

void Settings::setJson(std::string_view key, const nlohmann::json &value)
{
    std::string k(key);
    bool isChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_data.is_object()) {
            m_data = nlohmann::json::object();
        }
        if (!m_data.contains(k) || m_data[k] != value) {
            m_data[k] = value;
            isChanged = true;
        }
    }

    if (isChanged) {
        changed.emit(k, value);
    }
}

bool Settings::contains(std::string_view key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_data.is_object() && m_data.contains(std::string(key));
}

void Settings::remove(std::string_view key)
{
    std::string k(key);
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_data.is_object() && m_data.contains(k)) {
            m_data.erase(k);
            removed = true;
        }
    }
    if (removed) {
        changed.emit(k, nlohmann::json());
    }
}

void Settings::clear()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_data = nlohmann::json::object();
    }
}

size_t Settings::importAndApplyRegistry(bool saveAfter)
{
    auto imported = importFromRegistry();
    if (imported.empty()) {
        return 0;
    }
    for (const auto &[key, val] : imported) {
        setJson(key, val);
    }
    if (saveAfter) {
        save();
    }
    return imported.size();
}

} // namespace Styx::Platform
