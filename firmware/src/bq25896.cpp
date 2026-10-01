#include "bq25896.h"

#include "freertos/FreeRTOS.h"

namespace {
constexpr uint8_t kRegInputSource = 0x00;
constexpr uint8_t kRegAdcControl = 0x02;
constexpr uint8_t kRegChargeControl = 0x03;
constexpr uint8_t kRegPowerPath = 0x03;
constexpr uint8_t kRegChargeCurrent = 0x04;
constexpr uint8_t kRegWatchdog = 0x07;
constexpr uint8_t kRegStatusFault = 0x09;
constexpr uint8_t kRegChargeVoltage = 0x06;
constexpr uint8_t kRegStatus = 0x0B;
constexpr uint8_t kRegFault = 0x0C;
constexpr uint8_t kRegBatteryVoltage = 0x0E;
constexpr uint8_t kRegChargeCurrentAdc = 0x12;
constexpr uint8_t kRegPartNumber = 0x14;
constexpr uint8_t kInputCurrentMask = 0x1F;
constexpr uint8_t kChargeCurrentMask = 0x7F;
constexpr uint8_t kChargeVoltageMask = 0xFC;
constexpr uint8_t kChargeEnableMask = 0x10;
constexpr uint8_t kChargeEnableValue = 0x10;
constexpr uint8_t kWatchdogResetMask = 0x40;
constexpr uint8_t kWatchdogFieldMask = 0x30;
constexpr uint8_t kBatteryFetDisableMask = 0x20;
constexpr uint8_t kSystemMinVoltageMask = 0x0E;
constexpr uint8_t kChargeStatusMask = 0x18;
constexpr uint8_t kPartNumberMask = 0x78;
constexpr uint8_t kBatteryVoltageMask = 0x7F;
constexpr uint8_t kAdcConversionMask = 0xC0;
}

BQ::BQ(i2c_master_bus_handle_t bus, uint8_t address)
    : bus_(bus), device_(nullptr), address_(address)
{
}

esp_err_t BQ::BQ_begin()
{
    esp_err_t ret = BQ_ensure_device();
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t part_number = 0;
    ret = BQ_get_part_number(&part_number);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = BQ_update_register(kRegAdcControl, kAdcConversionMask, kAdcConversionMask);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(1500));
    return ESP_OK;
}

esp_err_t BQ::BQ_read_register(uint8_t reg, uint8_t *value) const
{
    if (device_ == nullptr || value == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit_receive(
        device_, &reg, sizeof(reg), value, sizeof(*value), pdMS_TO_TICKS(100));
}

esp_err_t BQ::BQ_write_register(uint8_t reg, uint8_t value)
{
    if (device_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t payload[] = {reg, value};
    return i2c_master_transmit(device_, payload, sizeof(payload), pdMS_TO_TICKS(100));
}

esp_err_t BQ::BQ_update_register(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    esp_err_t ret = BQ_read_register(reg, &current);
    if (ret != ESP_OK) {
        return ret;
    }

    current = static_cast<uint8_t>((current & ~mask) | (value & mask));
    return BQ_write_register(reg, current);
}

esp_err_t BQ::BQ_set_charge_current_ma(uint16_t milliamps)
{
    if (milliamps > 5056) {
        return ESP_ERR_INVALID_ARG;
    }
    return BQ_update_register(kRegChargeCurrent, kChargeCurrentMask,
                              static_cast<uint8_t>(milliamps / 64));
}

esp_err_t BQ::BQ_set_charge_voltage_mv(uint16_t millivolts)
{
    if (millivolts < 3840 || millivolts > 4608) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t value = static_cast<uint8_t>((millivolts - 3840) / 16) << 2;
    return BQ_update_register(kRegChargeVoltage, kChargeVoltageMask, value);
}

esp_err_t BQ::BQ_set_input_current_limit_ma(uint16_t milliamps)
{
    if (milliamps > 3250) {
        return ESP_ERR_INVALID_ARG;
    }
    return BQ_update_register(kRegInputSource, kInputCurrentMask,
                              static_cast<uint8_t>(milliamps / 50));
}

esp_err_t BQ::BQ_set_charging_enabled(bool enabled)
{
    return BQ_update_register(kRegChargeControl, kChargeEnableMask,
                              enabled ? kChargeEnableValue : 0);
}

esp_err_t BQ::BQ_pet_watchdog()
{
    return BQ_update_register(kRegChargeControl, kWatchdogResetMask, kWatchdogResetMask);
}

esp_err_t BQ::BQ_disable_watchdog()
{
    return BQ_update_register(kRegWatchdog, kWatchdogFieldMask, 0);
}

esp_err_t BQ::BQ_set_battery_fet_enabled(bool enabled)
{
    return BQ_update_register(kRegStatusFault, kBatteryFetDisableMask,
                              enabled ? 0 : kBatteryFetDisableMask);
}

esp_err_t BQ::BQ_shutdown_circuit()
{
    return BQ_set_battery_fet_enabled(false);
}

esp_err_t BQ::BQ_set_system_min_voltage_mv(uint16_t millivolts)
{
    if (millivolts < 3000 || millivolts > 3700 || millivolts % 100 != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t value = static_cast<uint8_t>((millivolts - 3000) / 100) << 1;
    return BQ_update_register(kRegPowerPath, kSystemMinVoltageMask, value);
}

esp_err_t BQ::BQ_get_charge_status(ChargeStatus *status) const
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t value = 0;
    esp_err_t ret = BQ_read_register(kRegStatus, &value);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint8_t state = (value & kChargeStatusMask) >> 3;
    *status = state <= 3 ? static_cast<ChargeStatus>(state) : ChargeStatus::Unknown;
    return ESP_OK;
}

esp_err_t BQ::BQ_get_fault(uint8_t *fault) const
{
    if (fault == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t value = 0;
    esp_err_t ret = BQ_read_register(kRegFault, &value);
    if (ret == ESP_OK) {
        *fault = value;
    }
    return ret;
}

esp_err_t BQ::BQ_get_part_number(uint8_t *part_number) const
{
    if (part_number == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t value = 0;
    esp_err_t ret = BQ_read_register(kRegPartNumber, &value);
    if (ret == ESP_OK) {
        *part_number = (value & kPartNumberMask) >> 3;
    }
    return ret;
}

esp_err_t BQ::BQ_get_battery_voltage_mv(uint16_t *millivolts) const
{
    if (millivolts == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t value = 0;
    esp_err_t ret = BQ_read_register(kRegBatteryVoltage, &value);
    if (ret == ESP_OK) {
        const uint8_t raw_voltage = value & kBatteryVoltageMask;
        if (raw_voltage == 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *millivolts = static_cast<uint16_t>(2304 + raw_voltage * 20);
    }
    return ret;
}

esp_err_t BQ::BQ_get_charge_current_ma(uint16_t *milliamps) const
{
    if (milliamps == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t value = 0;
    esp_err_t ret = BQ_read_register(kRegChargeCurrentAdc, &value);
    if (ret == ESP_OK) {
        *milliamps = static_cast<uint16_t>((value & 0x7F) * 50);
    }
    return ret;
}

esp_err_t BQ::BQ_get_battery_percentage(uint8_t *percentage) const
{
    if (percentage == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t millivolts = 0;
    esp_err_t ret = BQ_get_battery_voltage_mv(&millivolts);
    if (ret != ESP_OK) {
        return ret;
    }

    return BQ_get_battery_percentage(millivolts, percentage);
}

esp_err_t BQ::BQ_get_battery_percentage(uint16_t millivolts, uint8_t *percentage) const
{
    if (percentage == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    struct VoltagePoint { uint16_t voltage; uint8_t percentage; };
    constexpr VoltagePoint curve[] = {
        {3600, 0}, {3700, 3}, {3750, 8}, {3800, 15}, {3850, 25},
        {3900, 40}, {3950, 55}, {4000, 70}, {4050, 80}, {4100, 90},
        {4150, 95}, {4200, 100},
    };
    constexpr size_t curve_size = sizeof(curve) / sizeof(curve[0]);
    if (millivolts <= curve[0].voltage) {
        *percentage = 0;
    } else if (millivolts >= curve[curve_size - 1].voltage) {
        *percentage = 100;
    } else {
        for (size_t index = 1; index < curve_size; ++index) {
            if (millivolts <= curve[index].voltage) {
                const VoltagePoint &lower = curve[index - 1];
                const VoltagePoint &upper = curve[index];
                *percentage = static_cast<uint8_t>(lower.percentage +
                    (millivolts - lower.voltage) * (upper.percentage - lower.percentage) /
                    (upper.voltage - lower.voltage));
                break;
            }
        }
    }
    return ESP_OK;
}

esp_err_t BQ::BQ_ensure_device()
{
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (device_ != nullptr) {
        return ESP_OK;
    }

    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address_,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };

    return i2c_master_bus_add_device(bus_, &config, &device_);
}