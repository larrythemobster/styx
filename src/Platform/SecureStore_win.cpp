#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#include <wincrypt.h>

#include "SecureStore.hpp"
#include "Utf8.hpp"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

namespace Styx::Platform {

static std::wstring getTargetName(const std::string &service, const std::string &key)
{
    // Legacy keychain on Windows directly uses the key as TargetName in Credential Manager.
    // If key is empty, fall back to service.
    std::string target = key.empty() ? service : key;
    return toUtf16(target);
}

bool SecureStore::writePassword(const std::string &service, const std::string &key, const std::string &password)
{
    std::wstring targetW = getTargetName(service, key);
    if (targetW.empty()) {
        return false;
    }

    CREDENTIALW cred = {};
    cred.Comment = const_cast<wchar_t *>(L"StyxSecureStore");
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<wchar_t *>(targetW.c_str());
    cred.Persist = CRED_PERSIST_ENTERPRISE;
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(password.data()));
    cred.CredentialBlobSize = static_cast<DWORD>(password.size());
    cred.AttributeCount = 0;
    cred.Attributes = nullptr;

    return CredWriteW(&cred, 0) != FALSE;
}

std::optional<std::string> SecureStore::readPassword(const std::string &service, const std::string &key)
{
    std::wstring targetW = getTargetName(service, key);
    if (targetW.empty()) {
        return std::nullopt;
    }

    PCREDENTIALW cred = nullptr;
    if (!CredReadW(targetW.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        return std::nullopt;
    }

    std::string result;
    if (cred->AttributeCount == 0) {
        result.assign(reinterpret_cast<const char *>(cred->CredentialBlob), cred->CredentialBlobSize);
    } else {
        // Handle chunked/encrypted data if generated for blobs > CRED_MAX_CREDENTIAL_BLOB_SIZE
        std::vector<BYTE> encrypted;
        encrypted.insert(encrypted.end(), cred->CredentialBlob, cred->CredentialBlob + cred->CredentialBlobSize);
        for (DWORD i = 0; i < cred->AttributeCount; ++i) {
            encrypted.insert(encrypted.end(), cred->Attributes[i].Value,
                             cred->Attributes[i].Value + cred->Attributes[i].ValueSize);
        }

        DATA_BLOB blobIn, blobOut;
        blobIn.pbData = encrypted.data();
        blobIn.cbData = static_cast<DWORD>(encrypted.size());

        if (CryptUnprotectData(&blobIn, nullptr, nullptr, nullptr, nullptr, 0, &blobOut)) {
            result.assign(reinterpret_cast<const char *>(blobOut.pbData), blobOut.cbData);
            SecureZeroMemory(blobOut.pbData, blobOut.cbData);
            LocalFree(blobOut.pbData);
        } else {
            CredFree(cred);
            return std::nullopt;
        }
    }

    CredFree(cred);
    return result;
}

bool SecureStore::deletePassword(const std::string &service, const std::string &key)
{
    std::wstring targetW = getTargetName(service, key);
    if (targetW.empty()) {
        return false;
    }

    if (!CredDeleteW(targetW.c_str(), CRED_TYPE_GENERIC, 0)) {
        DWORD err = GetLastError();
        if (err == ERROR_NOT_FOUND) {
            return true;
        }
        return false;
    }
    return true;
}

} // namespace Styx::Platform
#endif
