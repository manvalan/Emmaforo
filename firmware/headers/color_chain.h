#pragma once

#include <stddef.h>
#include <stdint.h>

#include "serial_rgb_led.h"

// A chain of colors the lamp fades through on its own. Each step fades from
// the previous color to its own in fade_ds tenths of a second, stays there
// hold_ds tenths, and plays its sound (audio_player.h numbering) when it arrives.
//
// Bluetooth FFEB, write:
//   0x00                      stop
//   0x02                      start the stored chain again
//   0x01 flags count steps…   store and start; flags bit0 = repeat
// Each step is 8 bytes: r, g, b, u16 fade_ds, u16 hold_ds, sound (LE).
// Read: running, current step, flags, count, steps…
class ColorChain {
public:
    static constexpr size_t kMaxSteps = 16;
    static constexpr size_t kStepBytes = 8;
    static constexpr size_t kMaxBytes = 3 + kMaxSteps * kStepBytes;

    struct Step {
        SerialRgbLed::Color color;
        uint16_t fade_ds;
        uint16_t hold_ds;
        uint8_t sound;
    };

    // Reads the chain stored in NVS. It does not start by itself.
    void load();
    // Applies a FFEB write. Returns false if it is malformed.
    bool apply(const uint8_t *data, size_t length, SerialRgbLed::Color current, int64_t now_us);
    void stop();
    bool running() const { return running_; }

    // Color to show now. sound is set to a step's sound the moment it is reached
    // (0 otherwise). Returns how many ms the caller can wait before asking again.
    uint32_t update(int64_t now_us, SerialRgbLed::Color *color, uint8_t *sound);
    // The color the chain is heading to, stable for logging.
    SerialRgbLed::Color target() const;
    size_t serialize(uint8_t *out, size_t size) const;

private:
    void start(SerialRgbLed::Color current, int64_t now_us);
    void save() const;

    Step steps_[kMaxSteps] = {};
    size_t count_ = 0;
    bool repeat_ = false;
    bool running_ = false;
    size_t index_ = 0;
    bool arrived_ = false;
    int64_t step_start_us_ = 0;
    SerialRgbLed::Color from_ = {};
};
