#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

// MAX17048G+ ModelGauge fuel gauge. One cell, no sense resistor, 7-bit 0x36
// on the shared I2C bus. Some boards are built without it: begin() reports
// ESP_ERR_NOT_FOUND and the caller keeps the BQ25896 estimate.
class Max17048 {
public:
    static constexpr uint8_t kAddress = 0x36;

    // STATUS register flags, high byte.
    static constexpr uint8_t kAlertReset = 0x01;        // RI: gauge powered up, not configured
    static constexpr uint8_t kAlertVoltageHigh = 0x02;  // VH
    static constexpr uint8_t kAlertVoltageLow = 0x04;   // VL
    static constexpr uint8_t kAlertVoltageReset = 0x08; // VR
    static constexpr uint8_t kAlertSocLow = 0x10;       // HD: SOC under the empty threshold
    static constexpr uint8_t kAlertSocChange = 0x20;    // SC
    static constexpr uint8_t kAlertMask = 0x3F;

    explicit Max17048(i2c_master_bus_handle_t bus);

    // Probes 0x36, checks the version register and applies the alert setup.
    esp_err_t begin(uint8_t empty_alert_percent, uint16_t low_alert_mv, uint16_t high_alert_mv);
    bool present() const { return present_; }
    uint16_t version() const { return version_; }

    esp_err_t read_voltage_mv(uint16_t *millivolts) const;
    // State of charge in 1/256 %. ModelGauge can report a little above 100%.
    esp_err_t read_soc_x256(uint16_t *soc) const;
    // Charge (+) or discharge (-) rate in 0.01 %/h.
    esp_err_t read_rate_centi_percent_per_hour(int16_t *rate) const;
    esp_err_t read_alerts(uint8_t *alerts) const;
    esp_err_t clear_alerts(uint8_t alerts);

private:
    esp_err_t read16(uint8_t reg, uint16_t *value) const;
    esp_err_t write16(uint8_t reg, uint16_t value);

    i2c_master_bus_handle_t bus_;
    i2c_master_dev_handle_t device_;
    bool present_;
    uint16_t version_;
};
