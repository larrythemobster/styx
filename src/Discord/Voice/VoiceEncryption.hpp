#pragma once

#include <string>

#include <cstdint>

#include "VoiceEnums.hpp"

namespace Styx {
namespace Discord {
namespace Voice {

class VoiceEncryption
{
public:
    VoiceEncryption(EncryptionMode mode, const std::string &secretKey);

    /// Encrypts an audio payload for an outbound RTP packet.
    /// @param rtpHeader  The serialized RTP header bytes (used as AAD for AEAD modes).
    /// @param audioPayload  The raw audio data (e.g. 3-byte Opus silence).
    /// @return The encrypted section to append after the RTP header:
    ///         [ciphertext + auth tag] + [4-byte supplemental nonce (big-endian)].
    ///         Empty on failure.
    std::string encrypt(const std::string &rtpHeader, const std::string &audioPayload);

    /// Decrypts the encrypted section of an inbound RTP packet.
    /// @param rtpHeader  The serialized RTP header bytes (used as AAD for AEAD modes).
    /// @param encryptedSection  Everything after the RTP header in the received packet.
    /// @return The decrypted audio payload, or empty on failure.
    std::string decrypt(const std::string &rtpHeader, const std::string &encryptedSection);

    /// Initialize libsodium. Must be called before any encrypt/decrypt operations.
    /// Safe to call multiple times.
    static bool initialize();

    /// Check whether the given encryption mode is supported on this hardware.
    /// Must call initialize() first.
    static bool isModeAvailable(EncryptionMode mode);

private:
    std::string encryptAes256Gcm(const std::string &rtpHeader, const std::string &payload);
    std::string encryptXChacha20(const std::string &rtpHeader, const std::string &payload);
    std::string decryptAes256Gcm(const std::string &rtpHeader, const std::string &encrypted);
    std::string decryptXChacha20(const std::string &rtpHeader, const std::string &encrypted);

    EncryptionMode m_mode;
    std::string m_key;
    uint32_t m_nonce = 0;
};

} // namespace Voice
} // namespace Discord
} // namespace Styx
