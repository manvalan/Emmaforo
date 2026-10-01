#include "setup_portal.h"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
SetupPortal *active_portal = nullptr;
TaskHandle_t app_task_handle = nullptr;
char pending_ssid[64] = {};
char pending_password[64] = {};
bool apply_requested = false;
bool charging_command_requested = false;
bool charging_command_enabled = true;
bool shutdown_command_requested = false;
bool led_command_requested = false;
SerialRgbLed::Color pending_led_color = {0, 0, 0};
uint8_t pending_led = 255;
bool pending_led_blink = false;
uint32_t pending_led_blink_interval_ms = 1000;
uint8_t pending_led_level = 255;

constexpr char kPage[] =
    "<!doctype html><html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Emmaforo Wi-Fi</title><style>body{font-family:sans-serif;max-width:560px;margin:32px auto;padding:0 16px}"
    "label,select,input,button{display:block;width:100%;box-sizing:border-box;margin:10px 0;padding:10px}"
    "button{cursor:pointer}.battery{font-size:1.5rem;margin:20px 0}</style><h1>Emmaforo</h1>"
    "<div class=battery>Batteria: <strong id=battery>--</strong> <small id=voltage></small><br>"
    "<button type=button onclick='charging(0)'>Metti in pausa la ricarica</button>"
    "<button type=button onclick='charging(1)'>Riprendi la ricarica</button>"
    "<button type=button onclick='shutdown()'>Spegni circuito</button></div>"
    "<form method=post action=/api/config><label>Rete Wi-Fi</label><select id=ssid name=ssid required></select>"
    "<label>Password</label><input name=password type=password minlength=8>"
    "<button type=button onclick=scan()>Cerca reti</button><button type=submit>Salva e collega</button></form>"
    "<script>async function battery(){let r=await fetch('/api/battery');let b=await r.json();document.querySelector('#battery').textContent=b.percentage+'%';document.querySelector('#voltage').textContent='('+b.voltage_mv+' mV)'}"
    "async function charging(e){await fetch('/api/charging',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'enabled='+e});battery()}"
    "async function shutdown(){await fetch('/api/shutdown',{method:'POST'});alert('Circuito spento')}"
    "async function scan(){let s=document.querySelector('#ssid');s.innerHTML='<option>Cerco...</option>';"
    "let r=await fetch('/api/scan');let a=await r.json();s.innerHTML='';a.forEach(x=>{let o=document.createElement('option');o.value=x.ssid;o.textContent=x.ssid+' ('+x.rssi+' dBm)';s.append(o)})}scan();battery();setInterval(battery,5000)</script></html>";

int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

void decode_value(const char *source, size_t length, char *target, size_t target_size)
{
    size_t output = 0;
    for (size_t index = 0; index < length && output + 1 < target_size; ++index) {
        if (source[index] == '+' && output + 1 < target_size) {
            target[output++] = ' ';
        } else if (source[index] == '%' && index + 2 < length) {
            const int high = hex_value(source[index + 1]);
            const int low = hex_value(source[index + 2]);
            if (high >= 0 && low >= 0) {
                target[output++] = static_cast<char>((high << 4) | low);
                index += 2;
            }
        } else {
            target[output++] = source[index];
        }
    }
    target[output] = '\0';
}

bool parameter(const char *body, const char *name, char *value, size_t value_size)
{
    const size_t name_length = std::strlen(name);
    const char *cursor = body;
    while (cursor != nullptr && *cursor != '\0') {
        if (std::strncmp(cursor, name, name_length) == 0 && cursor[name_length] == '=') {
            const char *start = cursor + name_length + 1;
            const char *end = std::strchr(start, '&');
            decode_value(start, end == nullptr ? std::strlen(start) : static_cast<size_t>(end - start), value, value_size);
            return true;
        }
        cursor = std::strchr(cursor, '&');
        if (cursor != nullptr) ++cursor;
    }
    return false;
}
}

SetupPortal::SetupPortal() : server_(nullptr), charger_(nullptr), rgb_led_(nullptr)
{
}

SetupPortal::~SetupPortal()
{
    stop();
}

esp_err_t SetupPortal::begin(BQ &charger, SerialRgbLed &rgb_led)
{
    if (server_ != nullptr) return ESP_OK;
    charger_ = &charger;
    rgb_led_ = &rgb_led;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t ret = httpd_start(&server_, &config);
    if (ret != ESP_OK) return ret;
    active_portal = this;
    app_task_handle = xTaskGetCurrentTaskHandle();

    httpd_uri_t index = {"/", HTTP_GET, index_handler, nullptr};
    httpd_uri_t scan = {"/api/scan", HTTP_GET, scan_handler, nullptr};
    httpd_uri_t configure = {"/api/config", HTTP_POST, config_handler, nullptr};
    httpd_uri_t battery = {"/api/battery", HTTP_GET, battery_handler, nullptr};
    httpd_uri_t charging = {"/api/charging", HTTP_POST, charging_handler, nullptr};
    httpd_uri_t shutdown = {"/api/shutdown", HTTP_POST, shutdown_handler, nullptr};
    httpd_uri_t led = {"/api/led", HTTP_POST, led_handler, nullptr};
    httpd_register_uri_handler(server_, &index);
    httpd_register_uri_handler(server_, &scan);
    httpd_register_uri_handler(server_, &configure);
    httpd_register_uri_handler(server_, &battery);
    httpd_register_uri_handler(server_, &charging);
    httpd_register_uri_handler(server_, &shutdown);
    return httpd_register_uri_handler(server_, &led);
}

esp_err_t SetupPortal::stop()
{
    active_portal = nullptr;
    if (server_ == nullptr) return ESP_OK;
    esp_err_t ret = httpd_stop(server_);
    server_ = nullptr;
    charger_ = nullptr;
    rgb_led_ = nullptr;
    return ret;
}

bool SetupPortal::take_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    if (!apply_requested || ssid == nullptr || password == nullptr || ssid_size == 0 || password_size == 0) {
        return false;
    }
    std::snprintf(ssid, ssid_size, "%s", pending_ssid);
    std::snprintf(password, password_size, "%s", pending_password);
    apply_requested = false;
    return true;
}

bool SetupPortal::take_charging_command(bool *enabled)
{
    if (!charging_command_requested || enabled == nullptr) return false;
    *enabled = charging_command_enabled;
    charging_command_requested = false;
    return true;
}

bool SetupPortal::take_shutdown_command()
{
    if (!shutdown_command_requested) return false;
    shutdown_command_requested = false;
    return true;
}

bool SetupPortal::take_led_command(SerialRgbLed::Color *color, uint8_t *led, bool *blink,
                                   uint32_t *blink_interval_ms, uint8_t *level)
{
    if (!led_command_requested || color == nullptr || led == nullptr || blink == nullptr ||
        blink_interval_ms == nullptr || level == nullptr) {
        return false;
    }
    *color = pending_led_color;
    *led = pending_led;
    *blink = pending_led_blink;
    *blink_interval_ms = pending_led_blink_interval_ms;
    *level = pending_led_level;
    led_command_requested = false;
    return true;
}

esp_err_t SetupPortal::index_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, kPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t SetupPortal::scan_handler(httpd_req_t *request)
{
    wifi_scan_config_t scan_config = {};
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi scan failed");

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    wifi_ap_record_t records[20] = {};
    uint16_t records_count = count > 20 ? 20 : count;
    esp_wifi_scan_get_ap_records(&records_count, records);

    char response[2048] = "[";
    for (uint16_t index = 0; index < records_count; ++index) {
        char item[128];
        std::snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                      index == 0 ? "" : ",", reinterpret_cast<char *>(records[index].ssid), records[index].rssi);
        std::strncat(response, item, sizeof(response) - std::strlen(response) - 1);
    }
    std::strncat(response, "]", sizeof(response) - std::strlen(response) - 1);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

esp_err_t SetupPortal::config_handler(httpd_req_t *request)
{
    char body[256] = {};
    const int received = httpd_req_recv(request, body, sizeof(body) - 1);
    if (received <= 0 || !parameter(body, "ssid", pending_ssid, sizeof(pending_ssid))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing SSID");
    }
    parameter(body, "password", pending_password, sizeof(pending_password));
    apply_requested = true;
    if (app_task_handle != nullptr) xTaskNotifyGive(app_task_handle);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, "Configurazione ricevuta. Riavvio della connessione...");
}

esp_err_t SetupPortal::battery_handler(httpd_req_t *request)
{
    if (active_portal == nullptr || active_portal->charger_ == nullptr) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Charger unavailable");
    }
    const PublishedBattery published = emmaforo_published_battery();
    if (!published.valid && published.voltage_mv == 0) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Battery unavailable");
    }
    char response[160];
    std::snprintf(response, sizeof(response),
                  "{\"percentage\":%u,\"voltage_mv\":%u,\"charging\":%s,\"charge_status\":%u,\"fault\":%u,\"estimated\":%s}",
                  published.percentage, published.voltage_mv, published.charging ? "true" : "false",
                  published.charge_status, published.fault, published.valid ? "true" : "false");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

esp_err_t SetupPortal::charging_handler(httpd_req_t *request)
{
    char body[32] = {};
    char enabled[8] = {};
    const int received = httpd_req_recv(request, body, sizeof(body) - 1);
    if (received <= 0 || !parameter(body, "enabled", enabled, sizeof(enabled))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing enabled");
    }
    charging_command_enabled = enabled[0] == '1';
    charging_command_requested = true;
    if (app_task_handle != nullptr) xTaskNotifyGive(app_task_handle);
    return httpd_resp_sendstr(request, "OK");
}

esp_err_t SetupPortal::shutdown_handler(httpd_req_t *request)
{
    shutdown_command_requested = true;
    if (app_task_handle != nullptr) xTaskNotifyGive(app_task_handle);
    return httpd_resp_sendstr(request, "Spegnimento circuito richiesto");
}

esp_err_t SetupPortal::led_handler(httpd_req_t *request)
{
    if (active_portal == nullptr || active_portal->rgb_led_ == nullptr) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "LED unavailable");
    }

    char query[160] = {};
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0 || query_length >= sizeof(query) ||
        httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing LED command");
    }

    char command[16] = {};
    char led_value[8] = {};
    char blink_value[8] = {};
    char interval_value[12] = {};
    char level_value[8] = {};
    if (!parameter(query, "command", command, sizeof(command))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing color");
    }
    parameter(query, "led", led_value, sizeof(led_value));
    parameter(query, "blink", blink_value, sizeof(blink_value));
    parameter(query, "interval", interval_value, sizeof(interval_value));
    parameter(query, "level", level_value, sizeof(level_value));

    if (std::strcmp(command, "rosso") == 0 || std::strcmp(command, "red") == 0) {
        pending_led_color = {255, 0, 0};
    } else if (std::strcmp(command, "giallo") == 0 || std::strcmp(command, "yellow") == 0) {
        pending_led_color = {255, 180, 0};
    } else if (std::strcmp(command, "verde") == 0 || std::strcmp(command, "green") == 0) {
        pending_led_color = {0, 255, 0};
    } else if (std::strcmp(command, "spento") == 0 || std::strcmp(command, "off") == 0) {
        pending_led_color = {0, 0, 0};
    } else if (command[0] == '#' && std::strlen(command) == 7) {
        unsigned red = 0;
        unsigned green = 0;
        unsigned blue = 0;
        if (std::sscanf(command, "#%02x%02x%02x", &red, &green, &blue) != 3) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Unknown color");
        }
        pending_led_color = {static_cast<uint8_t>(red), static_cast<uint8_t>(green),
                             static_cast<uint8_t>(blue)};
    } else {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Unknown color");
    }
    if (level_value[0] == '\0') {
        pending_led_level = 255;
    } else {
        const unsigned long level = std::strtoul(level_value, nullptr, 10);
        pending_led_level = static_cast<uint8_t>(level > 255 ? 255 : level);
    }

    if (std::strcmp(led_value, "0") == 0) {
        pending_led = 0;
    } else if (std::strcmp(led_value, "1") == 0) {
        pending_led = 1;
    } else if (led_value[0] == '\0' || std::strcmp(led_value, "255") == 0) {
        pending_led = 255;
    } else {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid LED target");
    }
    pending_led_blink = std::strcmp(blink_value, "1") == 0 || std::strcmp(blink_value, "true") == 0;
    const unsigned long interval = std::strtoul(interval_value, nullptr, 10);
    if (interval_value[0] != '\0') {
        pending_led_blink_interval_ms = static_cast<uint32_t>(interval < 500 ? 500 : interval > 2500 ? 2500 : interval);
    }
    led_command_requested = true;
    emmaforo_release_wifi();
    if (app_task_handle != nullptr) xTaskNotifyGive(app_task_handle);
    return httpd_resp_sendstr(request, "OK");
}