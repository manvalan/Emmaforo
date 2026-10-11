#include "battery_status.h"

#include "battery_policy.h"

#include "esp_log.h"
#include "esp_timer.h"

namespace {
constexpr char kTag[] = "battery";
constexpr uint8_t kRegStatus = 0x0B;
constexpr uint8_t kPowerGoodMask = 0x04;
// LP603048: 900 mAh. One percent is 9 mAh.
constexpr uint32_t kCellCapacityMilliampHours = 900;
// The cell voltage stays high while the charger is attached, and for a
// while after it lets go. Do not replace the integrated percent in that moment.
constexpr int64_t kRestBeforeVoltageUs = 10LL * 60 * 1000000;
// 4150 mV is already 95% on the curve, and 4200 mV is 100%. That range is the
// charger plateau, not a rested cell. A lower reading after charging has
// stopped can become the first estimate.
constexpr uint16_t kChargerPlateauMillivolts = 4150;
constexpr int64_t kRelaxBeforeVoltageUs = 60LL * 1000000;
}

BatteryStatus Bq25896EstimateSource::read(BQ &charger, bool lamp_on, bool safe_mode)
{
    (void)safe_mode;
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
    status.power_good = power_good;

    const bool charging = status_ok && (charge_status == BQ::ChargeStatus::PreCharge ||
                                        charge_status == BQ::ChargeStatus::FastCharge);
    const bool charge_done = status_ok && charge_status == BQ::ChargeStatus::ChargeDone;
    status.charging = charging;

    uint16_t measured_ma = 0;
    const bool measured_ok = charging && charger.BQ_get_charge_current_ma(&measured_ma) == ESP_OK;
    if (measured_ok) status.charge_current_ma = measured_ma;

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
        charge_idle_since_us_ = 0;
        if (phase_ != Phase::Charging) {
            dt_ms = 0;
            if (valid_) {
                anchor_ = percent_;
                charge_milliamp_milliseconds_ = 0;
                phase_ = Phase::Charging;
                ESP_LOGI(kTag, "Charge estimate stays at %u%%", percent_);
            }
        }
        // The measured current follows the taper near full; the setpoint does
        // not, and would run the estimate ahead of the cell.
        uint16_t charge_ma = 0;
        bool charge_ok = measured_ok;
        if (charge_ok) {
            charge_ma = measured_ma;
        } else {
            charge_ok = charger.BQ_get_charge_current_setting_ma(&charge_ma) == ESP_OK;
        }
        if (phase_ == Phase::Charging && valid_ && charge_ok && charge_ma > 0 && dt_ms > 0) {
            charge_milliamp_milliseconds_ += static_cast<uint64_t>(charge_ma) * dt_ms;
            const uint64_t capacity_ms =
                static_cast<uint64_t>(kCellCapacityMilliampHours) * 3600000ULL;
            const uint64_t added = (charge_milliamp_milliseconds_ * 100ULL) / capacity_ms;
            uint32_t next = static_cast<uint32_t>(anchor_) + static_cast<uint32_t>(added);
            // The charger stops at the 95% point (battery_policy), never above.
            if (next > battery_policy::kRealFullPercent) next = battery_policy::kRealFullPercent;
            percent_ = static_cast<uint8_t>(next);
        }
    } else if (!power_known) {
        // Keep the last percent. A missed status read is not a rested cell.
    } else if (charge_done) {
        // Termination at the lowered regulation voltage is the 95% point:
        // a real calibration for the estimate after every full charge.
        unplugged_since_us_ = 0;
        if (charge_idle_since_us_ == 0) charge_idle_since_us_ = now_us;
        if (!valid_ || percent_ != battery_policy::kRealFullPercent) {
            ESP_LOGI(kTag, "Charge done: estimate set to the full point");
        }
        percent_ = battery_policy::kRealFullPercent;
        anchor_ = percent_;
        valid_ = true;
        phase_ = Phase::Holding;
        charge_milliamp_milliseconds_ = 0;
    } else if (power_good) {
        unplugged_since_us_ = 0;
        if (charge_idle_since_us_ == 0) charge_idle_since_us_ = now_us;
        if (valid_ && phase_ == Phase::Rested) {
            phase_ = Phase::Holding;
        }
        const bool relaxed = voltage_ok && voltage_mv < kChargerPlateauMillivolts &&
                             now_us - charge_idle_since_us_ >= kRelaxBeforeVoltageUs;
        if (!valid_ && !lamp_on && relaxed) {
            uint8_t rested = 0;
            if (charger.BQ_get_battery_percentage(voltage_mv, &rested) == ESP_OK) {
                percent_ = rested;
                anchor_ = rested;
                valid_ = true;
                phase_ = Phase::Holding;
                charge_milliamp_milliseconds_ = 0;
                ESP_LOGI(kTag, "Charge estimate from rested voltage %u mV is %u%%", voltage_mv, percent_);
            }
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
    status.soc_x10 = static_cast<uint16_t>(percent_) * 10;
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

void Bq25896EstimateSource::restore(uint8_t percent)
{
    if (percent > 100) percent = 100;
    percent_ = percent;
    anchor_ = percent;
    valid_ = true;
    phase_ = Phase::Holding;
    charge_milliamp_milliseconds_ = 0;
}

bool Bq25896EstimateSource::estimate(uint8_t *percent) const
{
    if (percent == nullptr || !valid_) return false;
    *percent = percent_;
    return true;
}

namespace {
// Two missed reads in a row (one minute at the normal loop) before the
// sample is taken from the estimate instead.
constexpr uint8_t kGaugeFailuresBeforeFallback = 2;
}

Max17048Source::Max17048Source(Max17048 &gauge, Bq25896EstimateSource &fallback)
    : gauge_(gauge), fallback_(fallback)
{
}

BatteryStatus Max17048Source::read(BQ &charger, bool lamp_on, bool safe_mode)
{
    uint16_t voltage_mv = 0;
    uint16_t soc = 0;
    int16_t rate = 0;
    const bool gauge_ok = gauge_.read_voltage_mv(&voltage_mv) == ESP_OK &&
                          gauge_.read_soc_x256(&soc) == ESP_OK && voltage_mv > 0;
    if (!gauge_ok) {
        if (failures_ < UINT8_MAX) failures_++;
        if (failures_ == kGaugeFailuresBeforeFallback) {
            ESP_LOGW(kTag, "Fuel gauge stopped answering: using the charger estimate");
        }
        if (failures_ >= kGaugeFailuresBeforeFallback || !valid_) {
            BatteryStatus status = fallback_.read(charger, lamp_on, safe_mode);
            charger.BQ_start_adc_conversion();
            return status;
        }
    } else if (failures_ >= kGaugeFailuresBeforeFallback) {
        ESP_LOGI(kTag, "Fuel gauge answers again");
    }

    BatteryStatus status = {};
    status.charge_state = 255;
    BQ::ChargeStatus charge_status = BQ::ChargeStatus::Unknown;
    const bool status_ok = charger.BQ_get_charge_status(&charge_status) == ESP_OK;
    if (status_ok) status.charge_state = static_cast<uint8_t>(charge_status);
    uint8_t status_register = 0;
    status.power_good = charger.BQ_read_register(kRegStatus, &status_register) == ESP_OK &&
                        (status_register & kPowerGoodMask) != 0;
    status.charging = status_ok && (charge_status == BQ::ChargeStatus::PreCharge ||
                                    charge_status == BQ::ChargeStatus::FastCharge);
    uint16_t charge_ma = 0;
    if (status.charging && charger.BQ_get_charge_current_ma(&charge_ma) == ESP_OK) {
        status.charge_current_ma = charge_ma;
    }
    // One ADC conversion per sample is enough for ICHGR; continuous
    // conversion would keep the charger ADC running on battery.
    charger.BQ_start_adc_conversion();

    if (gauge_ok) {
        failures_ = 0;
        const uint32_t tenths = (static_cast<uint32_t>(soc) * 10U + 128U) / 256U;
        status.soc_x10 = static_cast<uint16_t>(tenths > 1000U ? 1000U : tenths);
        percent_ = static_cast<uint8_t>((status.soc_x10 + 5U) / 10U);
        valid_ = true;
        if (gauge_.read_rate_centi_percent_per_hour(&rate) == ESP_OK) status.rate_centi_per_hour = rate;
        uint8_t alerts = 0;
        if (gauge_.read_alerts(&alerts) == ESP_OK && alerts != 0) {
            status.gauge_alerts = alerts;
            if (alerts != last_alerts_) {
                if (alerts & Max17048::kAlertSocLow) ESP_LOGW(kTag, "Fuel gauge: battery low (%u%%)", percent_);
                if (alerts & Max17048::kAlertVoltageLow) ESP_LOGW(kTag, "Fuel gauge: cell under the low voltage alert");
                if (alerts & Max17048::kAlertVoltageHigh) ESP_LOGW(kTag, "Fuel gauge: cell over the high voltage alert");
                if (alerts & Max17048::kAlertReset) ESP_LOGW(kTag, "Fuel gauge was reset: SOC restarts from voltage");
            }
            // Clear so a new crossing is reported. HD stays set while SOC is low.
            gauge_.clear_alerts(alerts);
        }
        last_alerts_ = alerts;
        status.voltage_mv = voltage_mv;
        status.gauge = true;
        fallback_.anchor(percent_);
    } else {
        uint16_t bq_mv = 0;
        if (charger.BQ_get_battery_voltage_mv(&bq_mv) == ESP_OK) status.voltage_mv = bq_mv;
        status.soc_x10 = static_cast<uint16_t>(percent_) * 10;
    }
    status.percent = percent_;
    status.valid = valid_;
    return status;
}

void Max17048Source::anchor(uint8_t percent)
{
    // The gauge is the reference. Only the fallback follows a forced value.
    fallback_.anchor(percent);
}

bool Max17048Source::estimate(uint8_t *percent) const
{
    if (percent == nullptr) return false;
    if (valid_ && failures_ < kGaugeFailuresBeforeFallback) {
        *percent = percent_;
        return true;
    }
    return fallback_.estimate(percent);
}
