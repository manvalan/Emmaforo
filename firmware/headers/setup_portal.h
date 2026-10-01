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

struct PublishedInfo {
    char name[32];
    char wifi[64];
    bool wifi_saved;
    bool password_saved;
    bool safe;
    bool paused;
    bool gentle;
    uint8_t colors;
    char serial[18];
    char firmware[32];
    uint8_t preset[25];
};

PublishedBattery emmaforo_published_battery();
PublishedInfo emmaforo_published_info();
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
    bool take_charge_current(bool *gentle);
    bool take_shutdown_command();
    bool take_led_command(SerialRgbLed::Color *color, uint8_t *led, bool *blink,
                          uint32_t *blink_interval_ms, uint8_t *level);

private:
    static esp_err_t index_handler(httpd_req_t *request);
    static esp_err_t scan_handler(httpd_req_t *request);
    static esp_err_t config_handler(httpd_req_t *request);
    static esp_err_t battery_handler(httpd_req_t *request);
    static esp_err_t charging_handler(httpd_req_t *request);
    static esp_err_t current_handler(httpd_req_t *request);
    static esp_err_t shutdown_handler(httpd_req_t *request);
    static esp_err_t led_handler(httpd_req_t *request);
    static esp_err_t info_handler(httpd_req_t *request);

    httpd_handle_t server_;
    BQ *charger_;
    SerialRgbLed *rgb_led_;
};