#include "remote_crypto.h"

#include <cstring>

#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"

namespace remote_crypto {

void derive(const uint8_t key[kKeySize], const char *label, uint8_t out[kKeySize])
{
    uint8_t buffer[64 + kKeySize] = {};
    const size_t label_length = std::strlen(label);
    std::memcpy(buffer, label, label_length);
    std::memcpy(buffer + label_length, key, kKeySize);
    mbedtls_sha256(buffer, label_length + kKeySize, out, 0);
}

size_t seal(const uint8_t key[kKeySize], uint64_t sequence, const char *text, uint8_t *out, size_t out_size)
{
    const size_t text_length = text == nullptr ? 0 : std::strlen(text);
    const size_t plain_length = 8 + text_length;
    if (out == nullptr || out_size < kOverhead + plain_length || plain_length > 512) return 0;
    uint8_t plain[8 + 512] = {};
    for (int index = 0; index < 8; ++index) plain[index] = static_cast<uint8_t>(sequence >> (8 * index));
    std::memcpy(plain + 8, text, text_length);

    out[0] = 0x01;
    esp_fill_random(out + 1, 12);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret == 0) {
        ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plain_length, out + 1, 12, nullptr, 0,
                                        plain, out + 13, 16, out + 13 + plain_length);
    }
    mbedtls_gcm_free(&gcm);
    return ret == 0 ? kOverhead + plain_length : 0;
}

bool open(const uint8_t key[kKeySize], const uint8_t *in, size_t length, uint64_t *sequence, char *text,
          size_t text_size)
{
    if (in == nullptr || length < kOverhead + 8 || in[0] != 0x01 || text == nullptr || text_size == 0) return false;
    const size_t plain_length = length - kOverhead;
    if (plain_length - 8 >= text_size || plain_length > 8 + 512) return false;
    uint8_t plain[8 + 512] = {};
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret == 0) {
        ret = mbedtls_gcm_auth_decrypt(&gcm, plain_length, in + 1, 12, nullptr, 0, in + 13 + plain_length, 16,
                                       in + 13, plain);
    }
    mbedtls_gcm_free(&gcm);
    if (ret != 0) return false;
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index) value = (value << 8) | plain[index];
    if (sequence != nullptr) *sequence = value;
    std::memcpy(text, plain + 8, plain_length - 8);
    text[plain_length - 8] = '\0';
    return true;
}

bool parse_hex_key(const char *hex, uint8_t out[kKeySize])
{
    if (hex == nullptr || std::strlen(hex) != kKeySize * 2) return false;
    for (size_t index = 0; index < kKeySize; ++index) {
        uint8_t byte = 0;
        for (int half = 0; half < 2; ++half) {
            const char c = hex[index * 2 + half];
            uint8_t nibble = 0;
            if (c >= '0' && c <= '9') nibble = static_cast<uint8_t>(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = static_cast<uint8_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = static_cast<uint8_t>(c - 'A' + 10);
            else return false;
            byte = static_cast<uint8_t>((byte << 4) | nibble);
        }
        out[index] = byte;
    }
    return true;
}

}
