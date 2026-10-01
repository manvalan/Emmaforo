#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

class BQ {
public:
    static constexpr uint8_t kDefaultAddress = 0x6B;

    enum class ChargeStatus : uint8_t {
        NotCharging = 0,
        PreCharge = 1,
        FastCharge = 2,
        ChargeDone = 3,
        Unknown = 255,
    };

    BQ(i2c_master_bus_handle_t bus, uint8_t address = kDefaultAddress);

    esp_err_t BQ_begin();
    esp_err_t BQ_read_register(uint8_t reg, uint8_t *value) const;
    esp_err_t BQ_write_register(uint8_t reg, uint8_t value);
    esp_err_t BQ_update_register(uint8_t reg, uint8_t mask, uint8_t value);
    esp_err_t BQ_set_charge_current_ma(uint16_t milliamps);
    esp_err_t BQ_set_charge_voltage_mv(uint16_t millivolts);
    esp_err_t BQ_set_input_current_limit_ma(uint16_t milliamps);
    esp_err_t BQ_set_charging_enabled(bool enabled);
    esp_err_t BQ_pet_watchdog();
    esp_err_t BQ_disable_watchdog();
    esp_err_t BQ_set_battery_fet_enabled(bool enabled);
    esp_err_t BQ_shutdown_circuit();
    esp_err_t BQ_set_system_min_voltage_mv(uint16_t millivolts);
    esp_err_t BQ_get_charge_status(ChargeStatus *status) const;
    esp_err_t BQ_get_fault(uint8_t *fault) const;
    esp_err_t BQ_get_part_number(uint8_t *part_number) const;
    esp_err_t BQ_get_battery_voltage_mv(uint16_t *millivolts) const;
    esp_err_t BQ_get_charge_current_ma(uint16_t *milliamps) const;
    esp_err_t BQ_get_battery_percentage(uint8_t *percentage) const;
    esp_err_t BQ_get_battery_percentage(uint16_t millivolts, uint8_t *percentage) const;

private:
    esp_err_t BQ_ensure_device();

    i2c_master_bus_handle_t bus_;
    i2c_master_dev_handle_t device_;
    uint8_t address_;
};