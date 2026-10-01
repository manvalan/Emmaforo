#pragma once

#include <stddef.h>

#include "esp_err.h"

struct NetworkSettings {
    char ssid[64];
    char password[64];
    char device_name[32];
    uint8_t safe_charging_mode;
    uint8_t gentle_charge;
    uint8_t charge_paused;
};

class SettingsStore {
public:
    static constexpr size_t kLampColorPresetSize = 25;

    esp_err_t load(NetworkSettings *settings) const;
    esp_err_t save(const NetworkSettings &settings) const;
    esp_err_t clear_network() const;
    esp_err_t load_colors(uint8_t *data, size_t size) const;
    esp_err_t save_colors(const uint8_t *data, size_t size) const;
};