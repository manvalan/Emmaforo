#pragma once

#include <stddef.h>
#include <stdint.h>

#include "battery_status.h"

// Battery samples for test devices, read over Bluetooth by a test build of the
// companion app. Little-endian, 16 bytes, the layout the app decodes.
struct __attribute__((packed)) BatteryLogSample {
    uint32_t time_s;              // Unix seconds if kFlagEpoch, else seconds since boot
    uint16_t voltage_mv;
    uint16_t soc_x10;             // 0.1 %
    int16_t rate_centi_per_hour;  // 0.01 %/h, gauge only
    uint16_t charge_current_ma;
    uint8_t flags;
    uint8_t charge_state;
    uint8_t led_load;             // mean of the three LED channels after brightness, 0..255
    uint8_t boot;                 // boot counter, low byte
};
static_assert(sizeof(BatteryLogSample) == 16, "BatteryLogSample is part of the BLE log format");

struct BatteryLogContext {
    bool lamp_on;
    uint8_t led_load;
    bool wifi_on;
    bool ble_connected;
    bool resting;
};

namespace battery_log {
constexpr uint8_t kFlagCharging = 0x01;
constexpr uint8_t kFlagPowerGood = 0x02;
constexpr uint8_t kFlagLampOn = 0x04;
constexpr uint8_t kFlagWifiOn = 0x08;
constexpr uint8_t kFlagBleConnected = 0x10;
constexpr uint8_t kFlagResting = 0x20;
constexpr uint8_t kFlagGauge = 0x40;
constexpr uint8_t kFlagEpoch = 0x80;

struct Summary {
    uint32_t count;
    uint32_t capacity;
    uint32_t period_s;
    uint32_t uptime_s;
    uint32_t epoch_now;  // 0 until the phone has set the time this boot
    uint8_t boot;
    bool enabled;
};

void init();
bool enabled();
// Called once per main loop. Stores a sample when the period has passed or
// when charging, power or the lamp changed state.
void record(const BatteryStatus &status, const BatteryLogContext &context);
void set_time(uint32_t unix_seconds);
void clear();
Summary summary();
// Copies up to max_samples starting at index (0 = oldest). Returns the count copied.
size_t copy(uint32_t index, BatteryLogSample *out, size_t max_samples);
}
