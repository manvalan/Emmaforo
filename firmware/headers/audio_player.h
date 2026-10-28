#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Speaker on the batch3 board: a PAM8302 class-D amplifier. ~SD (GPIO22)
// switches it on, the sigma-delta output on GPIO23 goes through 1 kΩ/10 nF
// into IN+. Samples are signed 8-bit at 8 kHz.
//
// Sounds are numbered: 0 none, 1-5 built-in tones, 16-20 the five voice
// messages the app writes into the "audio" partition. Every lamp state
// (rosso, giallo, verde, spento, altro) has one sound, played when the lamp
// changes to it; a color chain step carries its own.
namespace audio {

constexpr uint8_t kSoundNone = 0;
constexpr uint8_t kToneCount = 5;
constexpr uint8_t kVoiceFirst = 16;
constexpr uint8_t kVoiceSlots = 5;
constexpr uint32_t kSampleRate = 8000;
constexpr size_t kVoiceMaxBytes = 24576;

enum class LampState : uint8_t { Rosso = 0, Giallo, Verde, Spento, Altro, Count };

esp_err_t init();
bool enabled();

// Plays a sound by number. A sound already playing is cut short.
void play(uint8_t sound);
void play_for_state(LampState state);
void stop();
bool playing();

// Text commands written to FFEA: "vol:N", "map:<stato>:<suono>", "play:N",
// "stop", "clip:begin:<slot>:<bytes>", "clip:end:<slot>:<crc32 hex>",
// "clip:del:<slot>". Returns false if the command is not understood or fails.
bool handle_command(const char *command);
// FFE9: u32 offset (little-endian) then the samples of the clip being written.
bool handle_clip_data(const uint8_t *data, size_t length);
// FFEA read: "1\tvolume\tmap r,g,v,s,a\tvoice mask\tmax ms per voice\tpartition".
int format_status(char *text, size_t size);

}  // namespace audio
