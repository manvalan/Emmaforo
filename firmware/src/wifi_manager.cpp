#include "wifi_manager.h"

#include <cstring>
#include <cstdio>

#include "esp_log.h"
#include "esp_wifi_default.h"
#include "mdns.h"
#include "nvs_flash.h"

static const char *WIFI_TAG = "WIFI_MGR";

namespace {
bool mdns_ready = false;

void publish_emmaforo()
{
    if (!mdns_ready) {
        if (mdns_init() != ESP_OK) {
            ESP_LOGW(WIFI_TAG, "mDNS did not start");
            return;
        }
        mdns_hostname_set("emmaforo");
        mdns_instance_name_set("Emmaforo");
        mdns_ready = true;
    } else {
        mdns_service_remove("_http", "_tcp");
    }
    if (mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0) != ESP_OK) {
        ESP_LOGW(WIFI_TAG, "emmaforo.local was not announced");
        return;
    }
    ESP_LOGI(WIFI_TAG, "Announced emmaforo.local");
}

void unpublish_emmaforo()
{
    if (!mdns_ready) return;
    mdns_service_remove("_http", "_tcp");
}
}

WifiManager::WifiManager()
    : connected_(false), is_ap_mode_(false), station_configured_(false), radio_up_(false),
      driver_ready_(false), suppress_connect_(false), netif_(nullptr), sta_netif_(nullptr),
      owner_task_(xTaskGetCurrentTaskHandle()), wifi_any_handler_(nullptr), ip_handler_(nullptr)
{
}

WifiManager::~WifiManager()
{
}

void WifiManager::unregister_handlers()
{
    if (wifi_any_handler_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_any_handler_);
        wifi_any_handler_ = nullptr;
    }
    if (ip_handler_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_);
        ip_handler_ = nullptr;
    }
}

esp_err_t WifiManager::begin_ap(const char *ssid, const char *password, int channel)
{
    if (ssid == nullptr || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_ERR_INVALID_STATE) ret = ESP_OK;
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    if (netif_ != nullptr) {
        esp_netif_destroy(netif_);
        netif_ = nullptr;
    }

    netif_ = esp_netif_create_default_wifi_ap();
    if (netif_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    sta_netif_ = nullptr;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::event_handler, this, nullptr);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_config_t wifi_cfg = {};
    snprintf(reinterpret_cast<char *>(wifi_cfg.ap.ssid), sizeof(wifi_cfg.ap.ssid), "%s", ssid);
    wifi_cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
    wifi_cfg.ap.channel = static_cast<uint8_t>(channel);
    wifi_cfg.ap.max_connection = 4;

    if (password != nullptr && password[0] != '\0') {
        wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        if (std::strlen(password) < 8) {
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(reinterpret_cast<char *>(wifi_cfg.ap.password), sizeof(wifi_cfg.ap.password), "%s", password);
    } else {
        wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        return ret;
    }

    is_ap_mode_ = true;
    station_configured_ = false;
    connected_ = true;
    ssid_ = ssid;
    password_ = password == nullptr ? "" : password;
        ESP_LOGI(WIFI_TAG, "AP started: %s", ssid_.c_str());
    return ESP_OK;
}

esp_err_t WifiManager::ensure_driver()
{
    if (driver_ready_) return ESP_OK;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_ERR_INVALID_STATE) ret = ESP_OK;
    if (ret != ESP_OK) return ret;

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    if (netif_ == nullptr) {
        netif_ = esp_netif_create_default_wifi_sta();
        if (netif_ == nullptr) return ESP_ERR_NO_MEM;
        sta_netif_ = netif_;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (wifi_any_handler_ == nullptr) {
        ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::event_handler, this,
                                                  &wifi_any_handler_);
        if (ret != ESP_OK) return ret;
    }
    if (ip_handler_ == nullptr) {
        ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::event_handler, this,
                                                  &ip_handler_);
        if (ret != ESP_OK) return ret;
    }

    driver_ready_ = true;
    return ESP_OK;
}

esp_err_t WifiManager::begin_sta(const char *ssid, const char *password)
{
    if (ssid == nullptr || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_driver();
    if (ret != ESP_OK) return ret;

    wifi_config_t wifi_cfg = {};
    snprintf(reinterpret_cast<char *>(wifi_cfg.sta.ssid), sizeof(wifi_cfg.sta.ssid), "%s", ssid);
    snprintf(reinterpret_cast<char *>(wifi_cfg.sta.password), sizeof(wifi_cfg.sta.password), "%s", password == nullptr ? "" : password);
    wifi_cfg.sta.threshold.authmode = password == nullptr || password[0] == '\0'
        ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    suppress_connect_ = false;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    radio_up_ = true;
    const esp_err_t power_save = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (power_save != ESP_OK) {
        ESP_LOGW(WIFI_TAG, "Wi-Fi power save was not applied: %s", esp_err_to_name(power_save));
    }
    const esp_err_t connect_ret = esp_wifi_connect();
    if (connect_ret != ESP_OK && connect_ret != ESP_ERR_WIFI_CONN &&
        connect_ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(WIFI_TAG, "STA connect was not accepted yet: %s", esp_err_to_name(connect_ret));
    }

    is_ap_mode_ = false;
    station_configured_ = true;
    ssid_ = ssid;
    password_ = password == nullptr ? "" : password;
        ESP_LOGI(WIFI_TAG, "STA connect requested to %s", ssid_.c_str());
    return ESP_OK;
}

esp_err_t WifiManager::prepare_scan()
{
    esp_err_t ret = ensure_driver();
    if (ret != ESP_OK) return ret;
    if (radio_up_) return ESP_OK;

    suppress_connect_ = true;
    station_configured_ = false;
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    radio_up_ = true;
    ESP_LOGI(WIFI_TAG, "Wi-Fi radio started for scan");
    return ESP_OK;
}

esp_err_t WifiManager::begin_ap_sta(const char *sta_ssid, const char *sta_password,
                                    const char *ap_ssid, const char *ap_password, int channel)
{
    if (sta_ssid == nullptr || sta_ssid[0] == '\0' || ap_ssid == nullptr || ap_ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (ap_password != nullptr && ap_password[0] != '\0' && std::strlen(ap_password) < 8) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_ERR_INVALID_STATE) ret = ESP_OK;
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (ap_netif == nullptr || sta_netif == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    netif_ = ap_netif;
    sta_netif_ = sta_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &WifiManager::event_handler, this, nullptr);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &WifiManager::event_handler, this, nullptr);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_config_t ap_config = {};
    snprintf(reinterpret_cast<char *>(ap_config.ap.ssid), sizeof(ap_config.ap.ssid), "%s", ap_ssid);
    ap_config.ap.ssid_len = static_cast<uint8_t>(std::strlen(ap_ssid));
    ap_config.ap.channel = static_cast<uint8_t>(channel);
    ap_config.ap.max_connection = 4;
    if (ap_password != nullptr && ap_password[0] != '\0') {
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        snprintf(reinterpret_cast<char *>(ap_config.ap.password), sizeof(ap_config.ap.password), "%s", ap_password);
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config_t sta_config = {};
    snprintf(reinterpret_cast<char *>(sta_config.sta.ssid), sizeof(sta_config.sta.ssid), "%s", sta_ssid);
    snprintf(reinterpret_cast<char *>(sta_config.sta.password), sizeof(sta_config.sta.password), "%s",
             sta_password == nullptr ? "" : sta_password);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ret != ESP_OK) {
        return ret;
    }

    is_ap_mode_ = true;
    station_configured_ = true;
    connected_ = false;
    ssid_ = sta_ssid;
    password_ = sta_password == nullptr ? "" : sta_password;
    ESP_LOGI(WIFI_TAG, "AP+STA started: AP=%s, STA=%s", ap_ssid, sta_ssid);
    return ESP_OK;
}

esp_err_t WifiManager::stop()
{
    station_configured_ = false;
    suppress_connect_ = true;
    connected_ = false;
    is_ap_mode_ = false;
    if (!radio_up_) return ESP_OK;

    unpublish_emmaforo();
    esp_wifi_disconnect();
    esp_err_t ret = esp_wifi_stop();
    if (ret == ESP_ERR_WIFI_NOT_STARTED) ret = ESP_OK;
    radio_up_ = false;
    ESP_LOGI(WIFI_TAG, "Wi-Fi radio parked without unloading Bluetooth");
    return ret;
}

bool WifiManager::is_connected() const
{
    return connected_;
}

bool WifiManager::is_ap_mode() const
{
    return is_ap_mode_;
}

bool WifiManager::needs_configuration() const
{
    return !station_configured_ || !connected_;
}

std::string WifiManager::station_ip() const
{
    if (sta_netif_ == nullptr || !connected_) return {};
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(sta_netif_, &ip_info) != ESP_OK || ip_info.ip.addr == 0) return {};
    char address[16] = {};
    if (esp_ip4addr_ntoa(&ip_info.ip, address, sizeof(address)) == nullptr) return {};
    return address;
}

void WifiManager::event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    auto *self = static_cast<WifiManager *>(arg);
    if (self == nullptr) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!self->suppress_connect_) esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        self->connected_ = false;
        const auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        ESP_LOGW(WIFI_TAG, "Wi-Fi disconnected, reason=%d", event == nullptr ? -1 : event->reason);
        if (self->station_configured_ && !self->suppress_connect_) esp_wifi_connect();
        if (self->owner_task_ != nullptr) xTaskNotifyGive(self->owner_task_);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        self->connected_ = true;
        if (self->sta_netif_ != nullptr) esp_netif_set_hostname(self->sta_netif_, "emmaforo");
        const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        if (event != nullptr) {
            ESP_LOGI(WIFI_TAG, "Joined stored network at " IPSTR, IP2STR(&event->ip_info.ip));
        }
        publish_emmaforo();
        if (self->owner_task_ != nullptr) xTaskNotifyGive(self->owner_task_);
    }
}
