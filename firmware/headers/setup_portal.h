#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "bq25896.h"
#include "serial_rgb_led.h"

struct PublishedBattery {
    uint8_t percentage;
    uint16_t voltage_mv;
    bool charging;
    uint8_t charge_status;
    uint8_t fault;
    bool valid;
};

PublishedBattery emmaforo_published_battery();
void emmaforo_note_wifi_use();
void emmaforo_release_wifi();

class SetupPortal {
public:
    SetupPortal();
    ~SetupPortal();

    esp_err_t begin(BQ &charger, SerialRgbLed &rgb_led);
    esp_err_t stop();
    bool take_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size);
    bool take_charging_command(bool *enabled);
    bool take_shutdown_command();
    bool take_led_command(SerialRgbLed::Color *color, uint8_t *led, bool *blink,
                          uint32_t *blink_interval_ms, uint8_t *level);

private:
    static esp_err_t index_handler(httpd_req_t *request);
    static esp_err_t scan_handler(httpd_req_t *request);
    static esp_err_t config_handler(httpd_req_t *request);
    static esp_err_t battery_handler(httpd_req_t *request);
    static esp_err_t charging_handler(httpd_req_t *request);
    static esp_err_t shutdown_handler(httpd_req_t *request);
    static esp_err_t led_handler(httpd_req_t *request);

    httpd_handle_t server_;
    BQ *charger_;
    SerialRgbLed *rgb_led_;
};