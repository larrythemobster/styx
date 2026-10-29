#include "VoiceEncryption.hpp"

#include "Core/Logging.hpp"

#include <sodium.h>

#include <cstring>

namespace Styx {
namespace Discord {
namespace Voice {

static constexpr int SUPPLEMENTAL_NONCE_SIZE = 4;

static std::string nonceHex(const uint8_t *nonce, size_t size)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 3 - 1);
    for (size_t i = 0; i < size; ++i) {
        if (i) result.push_back(' ');
        result.push_back(hex[nonce[i] >> 4]);
        result.push_back(hex[nonce[i] & 15]);
    }
    return result;
}

VoiceEncryption::VoiceEncryption(EncryptionMode mode, const std::string &secretKey)
    : m_mode(mode), m_key(secretKey)
{
}

bool VoiceEncryption::initialize()
{
    static bool success = []() {
        int result = sodium_init();
        if (result < 0) {
            Core::log(Core::LogLevel::Error, LogVoice) << "Failed to initialize libsodium";
            return false;
        }
        return true;
    }();
    return success;
}

bool VoiceEncryption::isModeAvailable(EncryptionMode mode)
{
    switch (mode) {
    case EncryptionMode::AEAD_AES256_GCM_RTPSIZE:
        return crypto_aead_aes256gcm_is_available() != 0;
    case EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE:
        return true;
    default:
        return false;
    }
}

std::string VoiceEncryption::encrypt(const std::string &rtpHeader, const std::string &audioPayload)
{
    switch (m_mode) {
    case EncryptionMode::AEAD_AES256_GCM_RTPSIZE:
        return encryptAes256Gcm(rtpHeader, audioPayload);
    case EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE:
        return encryptXChacha20(rtpHeader, audioPayload);
    default:
        Core::log(Core::LogLevel::Warning, LogVoice) << "Encrypt called with unknown encryption mode";
        return {};
    }
}

std::string VoiceEncryption::encryptAes256Gcm(const std::string &rtpHeader, const std::string &payload)
{
    // Nonce: 4-byte BE counter padded to 12 bytes
    uint8_t nonce[crypto_aead_aes256gcm_NPUBBYTES] = {};
    nonce[0] = static_cast<uint8_t>(m_nonce >> 24);
    nonce[1] = static_cast<uint8_t>(m_nonce >> 16);
    nonce[2] = static_cast<uint8_t>(m_nonce >> 8);
    nonce[3] = static_cast<uint8_t>(m_nonce);

    // Ciphertext = payload + 16-byte auth tag
    std::string ciphertext(payload.size() + crypto_aead_aes256gcm_ABYTES, '\0');
    unsigned long long cipherLen = 0;

    int ret = crypto_aead_aes256gcm_encrypt(
            reinterpret_cast<unsigned char *>(ciphertext.data()), &cipherLen,
            reinterpret_cast<const unsigned char *>(payload.data()), payload.size(),
            reinterpret_cast<const unsigned char *>(rtpHeader.data()), rtpHeader.size(),
            nullptr, nonce,
            reinterpret_cast<const unsigned char *>(m_key.data()));

    if (ret != 0) {
        Core::log(Core::LogLevel::Warning, LogVoice) << "AES-256-GCM encrypt failed";
        return {};
    }

    ciphertext.resize(static_cast<int>(cipherLen));
    ciphertext.append(reinterpret_cast<const char *>(nonce), SUPPLEMENTAL_NONCE_SIZE);

    m_nonce++;
    return ciphertext;
}

std::string VoiceEncryption::encryptXChacha20(const std::string &rtpHeader, const std::string &payload)
{
    // Nonce: 4-byte BE counter padded to 24 bytes
    uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {};
    nonce[0] = static_cast<uint8_t>(m_nonce >> 24);
    nonce[1] = static_cast<uint8_t>(m_nonce >> 16);
    nonce[2] = static_cast<uint8_t>(m_nonce >> 8);
    nonce[3] = static_cast<uint8_t>(m_nonce);

    std::string ciphertext(payload.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long cipherLen = 0;

    int ret = crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char *>(ciphertext.data()), &cipherLen,
            reinterpret_cast<const unsigned char *>(payload.data()), payload.size(),
            reinterpret_cast<const unsigned char *>(rtpHeader.data()), rtpHeader.size(),
            nullptr, nonce,
            reinterpret_cast<const unsigned char *>(m_key.data()));

    if (ret != 0) {
        Core::log(Core::LogLevel::Warning, LogVoice) << "XChaCha20-Poly1305 encrypt failed";
        return {};
    }

    ciphertext.resize(static_cast<int>(cipherLen));
    ciphertext.append(reinterpret_cast<const char *>(nonce), SUPPLEMENTAL_NONCE_SIZE);

    m_nonce++;
    return ciphertext;
}

std::string VoiceEncryption::decrypt(const std::string &rtpHeader, const std::string &encryptedSection)
{
    switch (m_mode) {
    case EncryptionMode::AEAD_AES256_GCM_RTPSIZE:
        return decryptAes256Gcm(rtpHeader, encryptedSection);
    case EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE:
        return decryptXChacha20(rtpHeader, encryptedSection);
    default:
        return {};
    }
}

std::string VoiceEncryption::decryptAes256Gcm(const std::string &rtpHeader, const std::string &encrypted)
{
    int minSize = static_cast<int>(crypto_aead_aes256gcm_ABYTES) + SUPPLEMENTAL_NONCE_SIZE;
    if (encrypted.size() < minSize) {
        Core::log(Core::LogLevel::Debug, LogVoice) << "AES-GCM decrypt: too short, need" << minSize << "got" << encrypted.size();
        return {};
    }

    // Extract 4-byte supplemental nonce from end of packet, place at start of 12-byte nonce
    uint8_t nonce[crypto_aead_aes256gcm_NPUBBYTES] = {};
    std::memcpy(nonce, encrypted.data() + encrypted.size() - SUPPLEMENTAL_NONCE_SIZE,
                SUPPLEMENTAL_NONCE_SIZE);

    int cipherLen = encrypted.size() - SUPPLEMENTAL_NONCE_SIZE;
    std::string plaintext(cipherLen - static_cast<int>(crypto_aead_aes256gcm_ABYTES), '\0');
    unsigned long long plainLen = 0;

    int ret = crypto_aead_aes256gcm_decrypt(
            reinterpret_cast<unsigned char *>(plaintext.data()), &plainLen,
            nullptr,
            reinterpret_cast<const unsigned char *>(encrypted.data()), cipherLen,
            reinterpret_cast<const unsigned char *>(rtpHeader.data()), rtpHeader.size(),
            nonce,
            reinterpret_cast<const unsigned char *>(m_key.data()));

    if (ret != 0) {
        Core::log(Core::LogLevel::Debug, LogVoice) << "AES-GCM decrypt failed: cipherLen =" << cipherLen
                          << "aadLen =" << rtpHeader.size()
                          << "nonce =" << nonceHex(nonce, sizeof(nonce));
        return {};
    }

    plaintext.resize(static_cast<int>(plainLen));
    return plaintext;
}

std::string VoiceEncryption::decryptXChacha20(const std::string &rtpHeader, const std::string &encrypted)
{
    int minSize = static_cast<int>(crypto_aead_xchacha20poly1305_ietf_ABYTES) + SUPPLEMENTAL_NONCE_SIZE;
    if (encrypted.size() < minSize) {
        Core::log(Core::LogLevel::Debug, LogVoice) << "XChaCha20 decrypt: too short, need" << minSize << "got" << encrypted.size();
        return {};
    }

    // Extract 4-byte supplemental nonce from end of packet, place at start of 24-byte nonce
    uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {};
    std::memcpy(nonce, encrypted.data() + encrypted.size() - SUPPLEMENTAL_NONCE_SIZE,
                SUPPLEMENTAL_NONCE_SIZE);

    int cipherLen = encrypted.size() - SUPPLEMENTAL_NONCE_SIZE;
    std::string plaintext(cipherLen - static_cast<int>(crypto_aead_xchacha20poly1305_ietf_ABYTES), '\0');
    unsigned long long plainLen = 0;

    int ret = crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(plaintext.data()), &plainLen,
            nullptr,
            reinterpret_cast<const unsigned char *>(encrypted.data()), cipherLen,
            reinterpret_cast<const unsigned char *>(rtpHeader.data()), rtpHeader.size(),
            nonce,
            reinterpret_cast<const unsigned char *>(m_key.data()));

    if (ret != 0) {
        Core::log(Core::LogLevel::Debug, LogVoice) << "XChaCha20 decrypt failed: cipherLen =" << cipherLen
                          << "aadLen =" << rtpHeader.size()
                          << "nonce =" << nonceHex(nonce, sizeof(nonce));
        return {};
    }

    plaintext.resize(static_cast<int>(plainLen));
    return plaintext;
}

} // namespace Voice
} // namespace Discord
} // namespace Styx
