#include "firmware_update.h"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {
constexpr char kTag[] = "FW_UPDATE";
constexpr char kNamespace[] = "network";
constexpr char kApprovedKey[] = "fw_ok";
constexpr char kVersionURL[] = "https://emmaforo.michelebigi.it/firmware/version.txt";
constexpr char kImageURL[] = "https://emmaforo.michelebigi.it/firmware/emmaforo.bin";

char approved[32] = {};
bool busy = false;
bool attempt_now = false;
int64_t last_attempt_us = 0;

bool version_text(const char *version)
{
    if (version == nullptr || version[0] == '\0' || std::strlen(version) >= sizeof(approved)) {
        return false;
    }
    int dots = 0;
    bool digit = false;
    for (const char *cursor = version; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            if (!digit || dots == 1) return false;
            dots++;
            digit = false;
            continue;
        }
        if (*cursor < '0' || *cursor > '9') return false;
        digit = true;
    }
    return digit;
}

esp_err_t read_note_version(char *version, size_t version_size)
{
    esp_http_client_config_t config = {};
    config.url = kVersionURL;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 20000;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    char buffer[128] = {};
    const int read = esp_http_client_read_response(client, buffer, sizeof(buffer) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (status != 200 || read <= 0) return ESP_FAIL;

    char *line_end = std::strpbrk(buffer, "\r\n");
    if (line_end != nullptr) *line_end = '\0';
    if (!version_text(buffer)) return ESP_ERR_INVALID_VERSION;
    std::snprintf(version, version_size, "%s", buffer);
    return ESP_OK;
}

void update_task(void *)
{
#if CONFIG_PM_ENABLE
    esp_pm_lock_handle_t pm_lock = nullptr;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "fw_update", &pm_lock) == ESP_OK && pm_lock != nullptr) {
        esp_pm_lock_acquire(pm_lock);
    }
#endif

    char note[32] = {};
    const esp_err_t note_err = read_note_version(note, sizeof(note));
    if (note_err != ESP_OK || std::strcmp(note, approved) != 0 ||
        std::strcmp(note, firmware_version()) == 0) {
        ESP_LOGW(kTag, "Version note does not match the approved firmware");
        busy = false;
#if CONFIG_PM_ENABLE
        if (pm_lock != nullptr) esp_pm_lock_delete(pm_lock);
#endif
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_config_t http_config = {};
    http_config.url = kImageURL;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.timeout_ms = 60000;
    http_config.keep_alive_enable = true;
    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;
    ota_config.bulk_flash_erase = true;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK || handle == nullptr) {
        ESP_LOGE(kTag, "OTA begin failed: %s", esp_err_to_name(err));
        busy = false;
#if CONFIG_PM_ENABLE
        if (pm_lock != nullptr) esp_pm_lock_delete(pm_lock);
#endif
        vTaskDelete(nullptr);
        return;
    }

    esp_app_desc_t image = {};
    err = esp_https_ota_get_img_desc(handle, &image);
    if (err != ESP_OK || std::strcmp(image.version, approved) != 0) {
        ESP_LOGE(kTag, "Image is not the approved firmware");
        esp_https_ota_abort(handle);
        busy = false;
#if CONFIG_PM_ENABLE
        if (pm_lock != nullptr) esp_pm_lock_delete(pm_lock);
#endif
        vTaskDelete(nullptr);
        return;
    }

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(kTag, "OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        busy = false;
#if CONFIG_PM_ENABLE
        if (pm_lock != nullptr) esp_pm_lock_delete(pm_lock);
#endif
        vTaskDelete(nullptr);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OTA finish failed: %s", esp_err_to_name(err));
        busy = false;
#if CONFIG_PM_ENABLE
        if (pm_lock != nullptr) esp_pm_lock_delete(pm_lock);
#endif
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(kTag, "Approved firmware written, restarting");
    esp_restart();
}
}

const char *firmware_version()
{
    const esp_app_desc_t *description = esp_app_get_description();
    if (description == nullptr || description->version[0] == '\0') return "";
    return description->version;
}

void firmware_update_load()
{
    approved[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = sizeof(approved);
    if (nvs_get_str(handle, kApprovedKey, approved, &length) != ESP_OK || !version_text(approved)) {
        approved[0] = '\0';
    }
    nvs_close(handle);
    if (std::strcmp(approved, "2") == 0 && std::strcmp(firmware_version(), "1.0") == 0) {
        firmware_update_approve("1.0");
    }
}

esp_err_t firmware_update_approve(const char *version)
{
    if (!version_text(version)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, kApprovedKey, version);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    std::snprintf(approved, sizeof(approved), "%s", version);
    attempt_now = true;
    ESP_LOGI(kTag, "Approved firmware %s", approved);
    return ESP_OK;
}

bool firmware_update_pending()
{
    return approved[0] != '\0' && std::strcmp(approved, firmware_version()) != 0;
}

bool firmware_update_busy()
{
    return busy;
}

void firmware_update_poll(bool wifi_connected)
{
    if (busy || !wifi_connected || !firmware_update_pending()) return;
    const int64_t now = esp_timer_get_time();
    if (!attempt_now && last_attempt_us != 0 && now - last_attempt_us < 20LL * 1000000LL) return;
    attempt_now = false;
    last_attempt_us = now;
    busy = true;
    if (xTaskCreate(update_task, "fw_update", 20480, nullptr, 5, nullptr) != pdPASS) {
        busy = false;
        ESP_LOGE(kTag, "Could not start the firmware download");
    }
}

void firmware_update_mark_valid()
{
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(kTag, "Rollback confirm failed: %s", esp_err_to_name(err));
    }
}
