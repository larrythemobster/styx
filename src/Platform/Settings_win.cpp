#include "Settings.hpp"
#include "Utf8.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>

namespace Styx::Platform {

static void enumerateRegistryKey(HKEY hKey, const std::string &prefix,
                                 std::unordered_map<std::string, nlohmann::json> &out)
{
    // 1. Enumerate values under hKey
    DWORD valueIndex = 0;
    while (true) {
        wchar_t valueName[256];
        DWORD valueNameLen = 256;
        DWORD type = 0;
        DWORD dataSize = 0;

        // Query size and type first
        LONG res = RegEnumValueW(hKey, valueIndex, valueName, &valueNameLen, nullptr, &type, nullptr, &dataSize);
        if (res == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (res == ERROR_SUCCESS) {
            std::vector<BYTE> data(dataSize + 2, 0);
            valueNameLen = 256;
            res = RegEnumValueW(hKey, valueIndex, valueName, &valueNameLen, nullptr, &type, data.data(), &dataSize);
            if (res == ERROR_SUCCESS) {
                std::string leafName = fromUtf16(std::wstring_view(valueName, valueNameLen));
                std::string fullKey = prefix.empty() ? leafName : (prefix + "/" + leafName);

                if (type == REG_SZ || type == REG_EXPAND_SZ) {
                    const wchar_t *wstr = reinterpret_cast<const wchar_t *>(data.data());
                    size_t wlen = dataSize / sizeof(wchar_t);
                    if (wlen > 0 && wstr[wlen - 1] == L'\0') {
                        --wlen;
                    }
                    std::string strVal = fromUtf16(std::wstring_view(wstr, wlen));
                    if (strVal == "true") {
                        out[fullKey] = true;
                    } else if (strVal == "false") {
                        out[fullKey] = false;
                    } else {
                        out[fullKey] = strVal;
                    }
                } else if (type == REG_DWORD) {
                    if (dataSize >= sizeof(DWORD)) {
                        DWORD dw = *reinterpret_cast<const DWORD *>(data.data());
                        out[fullKey] = static_cast<int64_t>(dw);
                    }
                } else if (type == REG_QWORD) {
                    if (dataSize >= sizeof(uint64_t)) {
                        uint64_t qw = *reinterpret_cast<const uint64_t *>(data.data());
                        out[fullKey] = static_cast<int64_t>(qw);
                    }
                } else if (type == REG_BINARY) {
                    // Store binary as array of byte integers
                    std::vector<uint8_t> bytes(data.begin(), data.begin() + dataSize);
                    out[fullKey] = bytes;
                }
            }
        }
        ++valueIndex;
    }

    // 2. Enumerate subkeys under hKey
    DWORD subkeyIndex = 0;
    while (true) {
        wchar_t subkeyName[256];
        DWORD subkeyNameLen = 256;
        LONG res = RegEnumKeyExW(hKey, subkeyIndex, subkeyName, &subkeyNameLen, nullptr, nullptr, nullptr, nullptr);
        if (res == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (res == ERROR_SUCCESS) {
            HKEY hSubKey = nullptr;
            if (RegOpenKeyExW(hKey, subkeyName, 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
                std::string subLeaf = fromUtf16(std::wstring_view(subkeyName, subkeyNameLen));
                std::string nextPrefix = prefix.empty() ? subLeaf : (prefix + "/" + subLeaf);
                enumerateRegistryKey(hSubKey, nextPrefix, out);
                RegCloseKey(hSubKey);
            }
        }
        ++subkeyIndex;
    }
}

std::unordered_map<std::string, nlohmann::json> Settings::importFromRegistry(const std::wstring &rootKey)
{
    std::unordered_map<std::string, nlohmann::json> result;
    HKEY hRoot = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, rootKey.c_str(), 0, KEY_READ, &hRoot) == ERROR_SUCCESS) {
        enumerateRegistryKey(hRoot, "", result);
        RegCloseKey(hRoot);
    }
    return result;
}

} // namespace Styx::Platform

#endif
