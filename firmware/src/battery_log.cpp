#include "battery_log.h"

#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "BATT_LOG";

#if CONFIG_EMMAFORO_BATTERY_LOG
constexpr uint32_t kCapacity = CONFIG_EMMAFORO_BATTERY_LOG_CAPACITY;
constexpr uint32_t kPeriodS = CONFIG_EMMAFORO_BATTERY_LOG_PERIOD_S;
constexpr uint32_t kMinEventGapS = 10;
constexpr uint32_t kMagic = 0x454D4C31;  // "EML1"
constexpr uint8_t kStateFlags = battery_log::kFlagCharging | battery_log::kFlagPowerGood |
                                battery_log::kFlagLampOn | battery_log::kFlagResting;

struct Store {
    uint32_t magic;
    uint32_t capacity;
    uint32_t head;   // next slot to write
    uint32_t count;
    uint8_t boot;
    BatteryLogSample samples[kCapacity];
};

// Survives esp_restart(), panics and watchdog resets, so a crash during a long
// discharge test does not lose the run. Power loss clears it.
__NOINIT_ATTR Store store;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
int64_t epoch_offset_s = 0;  // unix - uptime, 0 when unknown
uint32_t last_sample_uptime_s = 0;
uint8_t last_flags = 0;
bool have_sample = false;

uint32_t uptime_s()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000LL);
}
#endif
}

namespace battery_log {

void init()
{
#if CONFIG_EMMAFORO_BATTERY_LOG
    if (store.magic != kMagic || store.capacity != kCapacity || store.head >= kCapacity ||
        store.count > kCapacity) {
        std::memset(&store, 0, sizeof(store));
        store.magic = kMagic;
        store.capacity = kCapacity;
        ESP_LOGI(kTag, "Battery log started: %lu samples every %lu s",
                 static_cast<unsigned long>(kCapacity), static_cast<unsigned long>(kPeriodS));
    } else {
        store.boot++;
        ESP_LOGI(kTag, "Battery log kept across reset: %lu samples", static_cast<unsigned long>(store.count));
    }
#endif
}

bool enabled()
{
#if CONFIG_EMMAFORO_BATTERY_LOG
    return true;
#else
    return false;
#endif
}

void record(const BatteryStatus &status, const BatteryLogContext &context)
{
#if CONFIG_EMMAFORO_BATTERY_LOG
    if (status.voltage_mv == 0 && status.charge_state == 255) return;
    uint8_t flags = 0;
    if (status.charging) flags |= kFlagCharging;
    if (status.power_good) flags |= kFlagPowerGood;
    if (context.lamp_on) flags |= kFlagLampOn;
    if (context.wifi_on) flags |= kFlagWifiOn;
    if (context.ble_connected) flags |= kFlagBleConnected;
    if (context.resting) flags |= kFlagResting;
    if (status.gauge) flags |= kFlagGauge;

    const uint32_t now = uptime_s();
    const uint32_t since = now - last_sample_uptime_s;
    const bool period_due = !have_sample || since >= kPeriodS;
    const bool state_changed = have_sample && (flags & kStateFlags) != (last_flags & kStateFlags) &&
                               since >= kMinEventGapS;
    if (!period_due && !state_changed) return;

    BatteryLogSample sample = {};
    if (epoch_offset_s != 0) {
        sample.time_s = static_cast<uint32_t>(epoch_offset_s + now);
        flags |= kFlagEpoch;
    } else {
        sample.time_s = now;
    }
    sample.voltage_mv = status.voltage_mv;
    sample.soc_x10 = status.valid ? status.soc_x10 : 0xFFFF;
    sample.rate_centi_per_hour = status.rate_centi_per_hour;
    sample.charge_current_ma = status.charge_current_ma;
    sample.flags = flags;
    sample.charge_state = status.charge_state;
    sample.led_load = context.lamp_on ? context.led_load : 0;

    taskENTER_CRITICAL(&lock);
    sample.boot = store.boot;
    store.samples[store.head] = sample;
    store.head = (store.head + 1) % kCapacity;
    if (store.count < kCapacity) store.count++;
    taskEXIT_CRITICAL(&lock);

    last_sample_uptime_s = now;
    last_flags = flags;
    have_sample = true;
#else
    (void)status;
    (void)context;
#endif
}

void set_time(uint32_t unix_seconds)
{
#if CONFIG_EMMAFORO_BATTERY_LOG
    // Anything before 2024 is not a phone clock.
    if (unix_seconds < 1704067200U) return;
    epoch_offset_s = static_cast<int64_t>(unix_seconds) - uptime_s();
#else
    (void)unix_seconds;
#endif
}

void clear()
{
#if CONFIG_EMMAFORO_BATTERY_LOG
    taskENTER_CRITICAL(&lock);
    store.head = 0;
    store.count = 0;
    taskEXIT_CRITICAL(&lock);
    have_sample = false;
    ESP_LOGI(kTag, "Battery log cleared");
#endif
}

Summary summary()
{
    Summary result = {};
#if CONFIG_EMMAFORO_BATTERY_LOG
    taskENTER_CRITICAL(&lock);
    result.count = store.count;
    result.boot = store.boot;
    taskEXIT_CRITICAL(&lock);
    result.capacity = kCapacity;
    result.period_s = kPeriodS;
    result.uptime_s = uptime_s();
    result.epoch_now = epoch_offset_s != 0 ? static_cast<uint32_t>(epoch_offset_s + result.uptime_s) : 0;
    result.enabled = true;
#endif
    return result;
}

size_t copy(uint32_t index, BatteryLogSample *out, size_t max_samples)
{
#if CONFIG_EMMAFORO_BATTERY_LOG
    if (out == nullptr) return 0;
    size_t copied = 0;
    taskENTER_CRITICAL(&lock);
    const uint32_t oldest = (store.head + kCapacity - store.count) % kCapacity;
    while (copied < max_samples && index + copied < store.count) {
        out[copied] = store.samples[(oldest + index + copied) % kCapacity];
        copied++;
    }
    taskEXIT_CRITICAL(&lock);
    return copied;
#else
    (void)index;
    (void)out;
    (void)max_samples;
    return 0;
#endif
}

}
