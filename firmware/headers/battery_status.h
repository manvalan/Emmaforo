#pragma once

#include <stdint.h>

#include "bq25896.h"
#include "max17048.h"

struct BatteryStatus {
    uint8_t percent;
    uint16_t voltage_mv;
    uint8_t charge_state;
    uint8_t fault;
    bool charging;
    bool valid;
    bool power_good;
    // Filled only when the MAX17048 answered this sample.
    bool gauge;
    uint16_t soc_x10;            // 0.1 %, also set from the estimate
    int16_t rate_centi_per_hour; // 0.01 %/h, + charging, - discharging
    uint8_t gauge_alerts;        // Max17048::kAlert* flags seen this sample
    uint16_t charge_current_ma;  // BQ25896 ICHGR ADC, 50 mA steps
};

class BatteryStatusSource {
public:
    virtual ~BatteryStatusSource() = default;
    virtual BatteryStatus read(BQ &charger, bool lamp_on, bool safe_mode) = 0;
    virtual void anchor(uint8_t percent) = 0;
    virtual bool estimate(uint8_t *percent) const = 0;
};

class Bq25896EstimateSource : public BatteryStatusSource {
public:
    BatteryStatus read(BQ &charger, bool lamp_on, bool safe_mode) override;
    void anchor(uint8_t percent) override;
    void restore(uint8_t percent);
    bool estimate(uint8_t *percent) const override;

private:
    enum class Phase : uint8_t { Unknown, Rested, Charging, Holding };

    uint8_t percent_ = 0;
    uint8_t anchor_ = 0;
    bool valid_ = false;
    Phase phase_ = Phase::Unknown;
    uint64_t charge_milliamp_milliseconds_ = 0;
    int64_t last_us_ = 0;
    int64_t unplugged_since_us_ = 0;
    int64_t charge_idle_since_us_ = 0;
};

// MAX17048G+ at 0x36 on the same bus. Used only when it answered at boot.
// The BQ25896 still reports charge state, power good and faults. If the gauge
// stops answering, each sample falls back to the estimate, which is kept
// anchored to the last gauge percent so the switch does not jump.
class Max17048Source : public BatteryStatusSource {
public:
    Max17048Source(Max17048 &gauge, Bq25896EstimateSource &fallback);
    BatteryStatus read(BQ &charger, bool lamp_on, bool safe_mode) override;
    void anchor(uint8_t percent) override;
    bool estimate(uint8_t *percent) const override;

private:
    Max17048 &gauge_;
    Bq25896EstimateSource &fallback_;
    uint8_t percent_ = 0;
    bool valid_ = false;
    uint8_t failures_ = 0;
    uint8_t last_alerts_ = 0;
};
