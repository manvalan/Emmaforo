#pragma once

#include <stdint.h>

#include "bq25896.h"
#include "max17048.h"

// Power measurements for test boards, compiled only with
// CONFIG_EMMAFORO_DEBUG_POWER; otherwise these calls do nothing. The board has
// no current sensor: the drain comes from the gauge (rate x 900 mAh).
//
// Each row is a line "PWR,..." on the USB serial port while USB is plugged in,
// and always a 32-byte record in the "powerlog" partition, which survives power
// loss. tools/power_log.py turns the partition into the same CSV.
namespace power_debug {

struct State {
    uint8_t r, g, b;  // LED after brightness
    bool lamp_on;
    bool blink;
    bool ble_connected;
    bool wifi_on;
    bool resting;
    uint8_t percent;  // used only on boards without a gauge
};

void begin(BQ *charger, Max17048 *gauge, uint8_t percent);
void set_state(const State &state);
void audio_started(uint8_t sound);
void audio_stopped();

}  // namespace power_debug
