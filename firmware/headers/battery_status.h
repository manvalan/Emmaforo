#pragma once

#include <stdint.h>

#include "bq25896.h"

struct BatteryStatus {
    uint8_t percent;
    uint16_t voltage_mv;
    uint8_t charge_state;
    uint8_t fault;
    bool charging;
    bool valid;
};

class BatteryStatusSource {
public:
    virtual ~BatteryStatusSource() = default;
    virtual BatteryStatus read(BQ &charger, bool lamp_on, bool safe_mode) = 0;
    virtual void anchor(uint8_t percent) = 0;
};

class Bq25896EstimateSource : public BatteryStatusSource {
public:
    BatteryStatus read(BQ &charger, bool lamp_on, bool safe_mode) override;
    void anchor(uint8_t percent) override;

private:
    uint8_t percent_ = 0;
    uint8_t anchor_ = 0;
    bool valid_ = false;
    bool integral_open_ = false;
    uint64_t charge_milliamp_milliseconds_ = 0;
    int64_t last_us_ = 0;
};

// Not fitted and not probed. A later MAX17048G+ can implement this and
// replace Bq25896EstimateSource at the one call site. One cell, no sense
// resistor, I2C 7-bit 0x36 on the existing bus. The charger stays at 0x6B.
class Max17048Source : public BatteryStatusSource {
public:
    static constexpr uint8_t kAddress = 0x36;
    BatteryStatus read(BQ &charger, bool lamp_on, bool safe_mode) override;
    void anchor(uint8_t percent) override;
};
