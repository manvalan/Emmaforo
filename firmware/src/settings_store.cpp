#include "settings_store.h"

#include <cstring>

#include "nvs.h"

namespace {
constexpr char kNamespace[] = "network";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";
constexpr char kDeviceNameKey[] = "device_name";
constexpr char kSafeChargingKey[] = "safe_charging";
constexpr char kGentleChargeKey[] = "charge_gentle";
constexpr char kChargePausedKey[] = "charge_paused";
constexpr char kLampColorsKey[] = "lamp_colors";
}

esp_err_t SettingsStore::load(NetworkSettings *settings) const
{
    if (settings == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    std::memset(settings, 0, sizeof(*settings));
    settings->safe_charging_mode = 1;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    size_t length = sizeof(settings->ssid);
    ret = nvs_get_str(handle, kSsidKey, settings->ssid, &length);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    length = sizeof(settings->password);
    esp_err_t password_ret = nvs_get_str(handle, kPasswordKey, settings->password, &length);
    if (password_ret != ESP_OK && password_ret != ESP_ERR_NVS_NOT_FOUND) {
        ret = password_ret;
    }
    length = sizeof(settings->device_name);
    esp_err_t name_ret = nvs_get_str(handle, kDeviceNameKey, settings->device_name, &length);
    if (name_ret != ESP_OK && name_ret != ESP_ERR_NVS_NOT_FOUND) {
        ret = name_ret;
    }
    uint8_t safe_charging = 1;
    esp_err_t safe_ret = nvs_get_u8(handle, kSafeChargingKey, &safe_charging);
    if (safe_ret == ESP_OK) settings->safe_charging_mode = safe_charging != 0;
    else if (safe_ret != ESP_ERR_NVS_NOT_FOUND) ret = safe_ret;
    uint8_t gentle_charge = 0;
    esp_err_t gentle_ret = nvs_get_u8(handle, kGentleChargeKey, &gentle_charge);
    if (gentle_ret == ESP_OK) settings->gentle_charge = gentle_charge != 0;
    else if (gentle_ret != ESP_ERR_NVS_NOT_FOUND) ret = gentle_ret;
    uint8_t charge_paused = 0;
    esp_err_t pause_ret = nvs_get_u8(handle, kChargePausedKey, &charge_paused);
    if (pause_ret == ESP_OK) settings->charge_paused = charge_paused != 0;
    else if (pause_ret != ESP_ERR_NVS_NOT_FOUND) ret = pause_ret;

    nvs_close(handle);
    return ret;
}

esp_err_t SettingsStore::save(const NetworkSettings &settings) const
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(handle, kSsidKey, settings.ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, kPasswordKey, settings.password);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, kDeviceNameKey, settings.device_name);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, kSafeChargingKey, settings.safe_charging_mode ? 1 : 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, kGentleChargeKey, settings.gentle_charge ? 1 : 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, kChargePausedKey, settings.charge_paused ? 1 : 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t SettingsStore::clear_network() const
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_err_t erase_ssid = nvs_erase_key(handle, kSsidKey);
    if (erase_ssid == ESP_ERR_NVS_NOT_FOUND) erase_ssid = ESP_OK;
    esp_err_t erase_password = nvs_erase_key(handle, kPasswordKey);
    if (erase_password == ESP_ERR_NVS_NOT_FOUND) erase_password = ESP_OK;
    ret = erase_ssid != ESP_OK ? erase_ssid : erase_password;
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t SettingsStore::load_colors(uint8_t *data, size_t size) const
{
    if (data == nullptr || size < kLampColorPresetSize) {
        return ESP_ERR_INVALID_ARG;
    }
    std::memset(data, 0, size);
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    size_t length = size;
    ret = nvs_get_blob(handle, kLampColorsKey, data, &length);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    nvs_close(handle);
    return ret;
}

esp_err_t SettingsStore::save_colors(const uint8_t *data, size_t size) const
{
    if (data == nullptr || size < kLampColorPresetSize) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_blob(handle, kLampColorsKey, data, kLampColorPresetSize);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}