#include "max17048.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace {
constexpr char kTag[] = "MAX17048";
constexpr int kTimeoutMs = 100;
constexpr uint8_t kRegVcell = 0x02;
constexpr uint8_t kRegSoc = 0x04;
constexpr uint8_t kRegVersion = 0x08;
constexpr uint8_t kRegConfig = 0x0C;
constexpr uint8_t kRegValrt = 0x14;
constexpr uint8_t kRegCrate = 0x16;
constexpr uint8_t kRegStatus = 0x1A;
// CONFIG low byte: SLEEP 0x80, ALSC 0x40, ALRT 0x20, ATHD 0x1F (empty = 32 - ATHD %).
constexpr uint16_t kConfigAlrt = 0x0020;
constexpr uint16_t kConfigAthdMask = 0x001F;
constexpr uint16_t kConfigSleep = 0x0080;
constexpr uint16_t kConfigAlsc = 0x0040;
}

Max17048::Max17048(i2c_master_bus_handle_t bus)
    : bus_(bus), device_(nullptr), present_(false), version_(0)
{
}

esp_err_t Max17048::begin(uint8_t empty_alert_percent, uint16_t low_alert_mv, uint16_t high_alert_mv)
{
    present_ = false;
    if (bus_ == nullptr) return ESP_ERR_INVALID_ARG;

    const esp_err_t probe = i2c_master_probe(bus_, kAddress, kTimeoutMs);
    if (probe != ESP_OK) {
        return probe == ESP_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_ERR_NOT_FOUND;
    }

    if (device_ == nullptr) {
        i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = kAddress,
            .scl_speed_hz = 400000,
            .scl_wait_us = 0,
            .flags = {
                .disable_ack_check = false,
            },
        };
        const esp_err_t added = i2c_master_bus_add_device(bus_, &config, &device_);
        if (added != ESP_OK) return added;
    }

    uint16_t version = 0;
    esp_err_t ret = read16(kRegVersion, &version);
    if (ret != ESP_OK) return ret;
    // MAX17048/MAX17049 report 0x001X. Anything else at 0x36 is not this gauge.
    if ((version & 0xFFF0) != 0x0010) {
        ESP_LOGW(kTag, "Device at 0x%02X reports version 0x%04X, not a MAX17048", kAddress, version);
        return ESP_ERR_NOT_SUPPORTED;
    }
    version_ = version;

    uint16_t config = 0;
    ret = read16(kRegConfig, &config);
    if (ret != ESP_OK) return ret;
    if (empty_alert_percent < 1) empty_alert_percent = 1;
    if (empty_alert_percent > 32) empty_alert_percent = 32;
    const uint16_t athd = static_cast<uint16_t>(32 - empty_alert_percent) & kConfigAthdMask;
    // Keep RCOMP (high byte), wake the gauge, no 1% change alert, clear ALRT.
    config = static_cast<uint16_t>((config & 0xFF00) | athd);
    config &= static_cast<uint16_t>(~(kConfigSleep | kConfigAlsc | kConfigAlrt));
    ret = write16(kRegConfig, config);
    if (ret != ESP_OK) return ret;

    const uint16_t valrt = static_cast<uint16_t>(((low_alert_mv / 20) << 8) | ((high_alert_mv / 20) & 0xFF));
    ret = write16(kRegValrt, valrt);
    if (ret != ESP_OK) return ret;

    uint8_t alerts = 0;
    if (read_alerts(&alerts) == ESP_OK && (alerts & kAlertReset) != 0) {
        ESP_LOGI(kTag, "Gauge was reset since the last boot: SOC starts from the cell voltage");
    }
    clear_alerts(kAlertMask);

    present_ = true;
    ESP_LOGI(kTag, "MAX17048 found at 0x%02X, version 0x%04X", kAddress, version_);
    return ESP_OK;
}

esp_err_t Max17048::read_voltage_mv(uint16_t *millivolts) const
{
    if (millivolts == nullptr) return ESP_ERR_INVALID_ARG;
    uint16_t raw = 0;
    const esp_err_t ret = read16(kRegVcell, &raw);
    if (ret != ESP_OK) return ret;
    // 78.125 uV per LSB.
    *millivolts = static_cast<uint16_t>((static_cast<uint32_t>(raw) * 5U) / 64U);
    return ESP_OK;
}

esp_err_t Max17048::read_soc_x256(uint16_t *soc) const
{
    if (soc == nullptr) return ESP_ERR_INVALID_ARG;
    return read16(kRegSoc, soc);
}

esp_err_t Max17048::read_rate_centi_percent_per_hour(int16_t *rate) const
{
    if (rate == nullptr) return ESP_ERR_INVALID_ARG;
    uint16_t raw = 0;
    const esp_err_t ret = read16(kRegCrate, &raw);
    if (ret != ESP_OK) return ret;
    // 0.208 %/h per LSB, signed.
    int32_t centi = (static_cast<int32_t>(static_cast<int16_t>(raw)) * 208) / 10;
    if (centi > INT16_MAX) centi = INT16_MAX;
    if (centi < INT16_MIN) centi = INT16_MIN;
    *rate = static_cast<int16_t>(centi);
    return ESP_OK;
}

esp_err_t Max17048::read_alerts(uint8_t *alerts) const
{
    if (alerts == nullptr) return ESP_ERR_INVALID_ARG;
    uint16_t status = 0;
    const esp_err_t ret = read16(kRegStatus, &status);
    if (ret == ESP_OK) *alerts = static_cast<uint8_t>((status >> 8) & kAlertMask);
    return ret;
}

esp_err_t Max17048::clear_alerts(uint8_t alerts)
{
    uint16_t status = 0;
    esp_err_t ret = read16(kRegStatus, &status);
    if (ret != ESP_OK) return ret;
    status = static_cast<uint16_t>(status & ~(static_cast<uint16_t>(alerts & kAlertMask) << 8));
    ret = write16(kRegStatus, status);
    if (ret != ESP_OK) return ret;
    // The ALRT pin stays low until the CONFIG ALRT bit is cleared too.
    uint16_t config = 0;
    ret = read16(kRegConfig, &config);
    if (ret == ESP_OK && (config & kConfigAlrt) != 0) {
        ret = write16(kRegConfig, static_cast<uint16_t>(config & ~kConfigAlrt));
    }
    return ret;
}

esp_err_t Max17048::read16(uint8_t reg, uint16_t *value) const
{
    if (device_ == nullptr || value == nullptr) return ESP_ERR_INVALID_STATE;
    uint8_t data[2] = {};
    const esp_err_t ret = i2c_master_transmit_receive(device_, &reg, sizeof(reg), data, sizeof(data),
                                                      kTimeoutMs);
    if (ret == ESP_OK) *value = static_cast<uint16_t>((data[0] << 8) | data[1]);
    return ret;
}

esp_err_t Max17048::write16(uint8_t reg, uint16_t value)
{
    if (device_ == nullptr) return ESP_ERR_INVALID_STATE;
    const uint8_t payload[] = {reg, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF)};
    return i2c_master_transmit(device_, payload, sizeof(payload), kTimeoutMs);
}
