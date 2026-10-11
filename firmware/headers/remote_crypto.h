#pragma once

#include <stddef.h>
#include <stdint.h>

// End-to-end protection for remote control. The key is created by the phone,
// written to the board over Bluetooth and never sent anywhere else, so the
// broker only relays random topic names and ciphertext it cannot read.
//
// Derivation: SHA-256(label || key) with the labels below.
// Message: 0x01 | 12-byte random nonce | AES-256-GCM ciphertext | 16-byte tag.
// Plaintext: u64 little-endian sequence | UTF-8 text.
namespace remote_crypto {
constexpr size_t kKeySize = 32;
constexpr size_t kOverhead = 1 + 12 + 16;
constexpr char kTopicLabel[] = "emmaforo-topic-v1";
constexpr char kCommandLabel[] = "emmaforo-cmd-v1";
constexpr char kStateLabel[] = "emmaforo-state-v1";

void derive(const uint8_t key[kKeySize], const char *label, uint8_t out[kKeySize]);
// Returns the sealed length, or 0 if out is too small.
size_t seal(const uint8_t key[kKeySize], uint64_t sequence, const char *text, uint8_t *out, size_t out_size);
// Returns true and the text when the tag checks. text is NUL-terminated.
bool open(const uint8_t key[kKeySize], const uint8_t *in, size_t length, uint64_t *sequence, char *text,
          size_t text_size);
bool parse_hex_key(const char *hex, uint8_t out[kKeySize]);
}
