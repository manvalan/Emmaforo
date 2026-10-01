#include "battery_status.h"

#include "esp_timer.h"

namespace {
constexpr uint8_t kSafeFullPercent = 95;
// LP603048: 900 mAh, 3.7 V nominal, 3.3 Wh.
constexpr uint32_t kCellCapacityMilliampHours = 900;
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

    const bool charging = status_ok && (charge_status == BQ::ChargeStatus::PreCharge ||
                                        charge_status == BQ::ChargeStatus::FastCharge);
    const bool charge_done = status_ok && charge_status == BQ::ChargeStatus::ChargeDone;
    status.charging = charging;

    uint16_t charge_current_ma = 0;
    const bool current_ok = charging && charger.BQ_get_charge_current_ma(&charge_current_ma) == ESP_OK;
    const int64_t now_us = esp_timer_get_time();
    uint32_t dt_ms = 0;
    if (last_us_ != 0 && now_us > last_us_) {
        const int64_t delta_ms = (now_us - last_us_) / 1000;
        if (delta_ms > 0 && delta_ms < 120000) dt_ms = static_cast<uint32_t>(delta_ms);
    }
    last_us_ = now_us;

    if (charge_done) {
        percent_ = safe_mode ? kSafeFullPercent : 100;
        anchor_ = percent_;
        valid_ = true;
        integral_open_ = false;
        charge_milliamp_milliseconds_ = 0;
    } else if (charging) {
        if (!integral_open_) {
            integral_open_ = true;
            charge_milliamp_milliseconds_ = 0;
            anchor_ = percent_;
        }
        if (current_ok && dt_ms > 0) {
            charge_milliamp_milliseconds_ += static_cast<uint64_t>(charge_current_ma) * dt_ms;
        }
        if (kCellCapacityMilliampHours > 0 && valid_) {
            const uint64_t added = (charge_milliamp_milliseconds_ * 100ULL) /
                (static_cast<uint64_t>(kCellCapacityMilliampHours) * 3600000ULL);
            uint32_t next = static_cast<uint32_t>(anchor_) + static_cast<uint32_t>(added);
            const uint32_t limit = safe_mode ? kSafeFullPercent : 100;
            if (next > limit) next = limit;
            percent_ = static_cast<uint8_t>(next);
        }
    } else if (!lamp_on && voltage_ok) {
        uint8_t rested = 0;
        if (charger.BQ_get_battery_percentage(voltage_mv, &rested) == ESP_OK) {
            percent_ = rested;
            anchor_ = rested;
            valid_ = true;
            integral_open_ = false;
            charge_milliamp_milliseconds_ = 0;
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
    integral_open_ = false;
    charge_milliamp_milliseconds_ = 0;
}
