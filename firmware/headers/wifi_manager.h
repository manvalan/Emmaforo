#pragma once

#include <string>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class WifiManager {
public:
    WifiManager();
    ~WifiManager();

    esp_err_t begin_ap(const char *ssid, const char *password, int channel = 1);
    esp_err_t begin_sta(const char *ssid, const char *password);
    esp_err_t begin_ap_sta(const char *sta_ssid, const char *sta_password,
                           const char *ap_ssid, const char *ap_password, int channel = 1);
    esp_err_t prepare_scan();
    esp_err_t stop();
    bool is_connected() const;
    bool is_ap_mode() const;
    bool needs_configuration() const;
    std::string station_ip() const;

private:
    static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
    void unregister_handlers();
    esp_err_t ensure_driver();

    bool connected_;
    bool is_ap_mode_;
    bool station_configured_;
    bool radio_up_;
    bool driver_ready_;
    bool suppress_connect_;
    esp_netif_t *netif_;
    esp_netif_t *sta_netif_;
    TaskHandle_t owner_task_;
    esp_event_handler_instance_t wifi_any_handler_;
    esp_event_handler_instance_t ip_handler_;
    std::string ssid_;
    std::string password_;
};
