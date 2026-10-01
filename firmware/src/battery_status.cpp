#include "battery_status.h"

#include "esp_log.h"
#include "esp_timer.h"

namespace {
constexpr char kTag[] = "battery";
constexpr uint8_t kSafeFullPercent = 95;
constexpr uint8_t kRegStatus = 0x0B;
constexpr uint8_t kPowerGoodMask = 0x04;
// LP603048: 900 mAh. One percent is 9 mAh.
constexpr uint32_t kCellCapacityMilliampHours = 900;
// The cell voltage stays high while the charger is attached, and for a
// while after it lets go. Do not replace the integrated percent in that moment.
constexpr int64_t kRestBeforeVoltageUs = 10LL * 60 * 1000000;
}

BatteryStatus Bq25896EstimateSource::read(BQ &charger, bool lamp_on, bool safe_mode)
{
    BatteryStatus status = {};
    status.percent = percent_;
    status.charge_state = 255;
    status.valid = valid_;

    uint16_t voltage_mv = 0;
    BQ::ChargeStatus charge_status = BQ::ChargeStatus::Unknown;
    const bool voltage_ok = charger.BQ_get_battery_voltage_mv(&voltage_mv) == ESP_OK;
    const bool status_ok = charger.BQ_get_charge_status(&charge_status) == ESP_OK;
    if (voltage_ok) status.voltage_mv = voltage_mv;
    if (status_ok) status.charge_state = static_cast<uint8_t>(charge_status);

    uint8_t status_register = 0;
    const bool power_known = charger.BQ_read_register(kRegStatus, &status_register) == ESP_OK;
    const bool power_good = power_known && (status_register & kPowerGoodMask) != 0;

    const bool charging = status_ok && (charge_status == BQ::ChargeStatus::PreCharge ||
                                        charge_status == BQ::ChargeStatus::FastCharge);
    const bool charge_done = status_ok && charge_status == BQ::ChargeStatus::ChargeDone;
    status.charging = charging;

    const int64_t now_us = esp_timer_get_time();
    uint32_t dt_ms = 0;
    if (last_us_ != 0 && now_us > last_us_) {
        const int64_t delta_ms = (now_us - last_us_) / 1000;
        if (delta_ms > 0 && delta_ms < 120000) dt_ms = static_cast<uint32_t>(delta_ms);
    }
    last_us_ = now_us;

    if (phase_ == Phase::Charging && !charging) {
        phase_ = Phase::Holding;
        charge_milliamp_milliseconds_ = 0;
        if (valid_) {
            ESP_LOGI(kTag, "Charge estimate holds %u%%", percent_);
        }
    }

    if (charging) {
        unplugged_since_us_ = 0;
        if (phase_ != Phase::Charging) {
            dt_ms = 0;
            if (valid_) {
                anchor_ = percent_;
                charge_milliamp_milliseconds_ = 0;
                phase_ = Phase::Charging;
                ESP_LOGI(kTag, "Charge estimate stays at %u%%", percent_);
            }
        }
        uint16_t setpoint_ma = 0;
        const bool setpoint_ok = charger.BQ_get_charge_current_setting_ma(&setpoint_ma) == ESP_OK;
        if (phase_ == Phase::Charging && valid_ && setpoint_ok && setpoint_ma > 0 && dt_ms > 0) {
            charge_milliamp_milliseconds_ += static_cast<uint64_t>(setpoint_ma) * dt_ms;
            const uint64_t capacity_ms =
                static_cast<uint64_t>(kCellCapacityMilliampHours) * 3600000ULL;
            const uint64_t added = (charge_milliamp_milliseconds_ * 100ULL) / capacity_ms;
            uint32_t next = static_cast<uint32_t>(anchor_) + static_cast<uint32_t>(added);
            const uint32_t limit = safe_mode ? kSafeFullPercent : 100;
            if (next > limit) next = limit;
            percent_ = static_cast<uint8_t>(next);
        }
    } else if (!power_known) {
        // Keep the last percent. A missed status read is not a rested cell.
    } else if (power_good || charge_done) {
        unplugged_since_us_ = 0;
        if (valid_ && phase_ == Phase::Rested) {
            phase_ = Phase::Holding;
        }
    } else {
        if (unplugged_since_us_ == 0) unplugged_since_us_ = now_us;
        const bool rested_long_enough = now_us - unplugged_since_us_ >= kRestBeforeVoltageUs;
        const bool initial = !valid_;
        const bool update_rested = phase_ == Phase::Rested;
        const bool release_hold = phase_ == Phase::Holding && rested_long_enough;
        if (!lamp_on && voltage_ok && (initial || update_rested || release_hold)) {
            uint8_t rested = 0;
            if (charger.BQ_get_battery_percentage(voltage_mv, &rested) == ESP_OK) {
                percent_ = rested;
                anchor_ = rested;
                valid_ = true;
                phase_ = Phase::Rested;
                charge_milliamp_milliseconds_ = 0;
            }
        }
    }

    status.percent = percent_;
    status.valid = valid_;
    return status;
}

void Bq25896EstimateSource::anchor(uint8_t percent)
{
    if (percent > 100) percent = 100;
    percent_ = percent;
    anchor_ = percent;
    valid_ = true;
    phase_ = Phase::Holding;
    charge_milliamp_milliseconds_ = 0;
}
