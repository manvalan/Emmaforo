#include <stdio.h>
#include <stdint.h>
#include <cstdlib>
#include <cstring>
#include "driver/i2c_master.h"
#include "bq25896.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "battery_status.h"
#include "bluetooth_manager.h"
#include "firmware_update.h"
#include "serial_rgb_led.h"
#include "settings_store.h"
#include "setup_portal.h"
#include "wifi_manager.h"

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool parse_lamp_color(const char *text, SerialRgbLed::Color *color)
{
    if (text == nullptr || color == nullptr) return false;
    if (std::strcmp(text, "rosso") == 0) { *color = {255, 0, 0}; return true; }
    if (std::strcmp(text, "giallo") == 0) { *color = {255, 180, 0}; return true; }
    if (std::strcmp(text, "verde") == 0) { *color = {0, 255, 0}; return true; }
    if (std::strcmp(text, "spento") == 0) { *color = {0, 0, 0}; return true; }
    if (text[0] != '#' || std::strlen(text) != 7) return false;
    uint8_t channels[3] = {};
    for (int index = 0; index < 3; ++index) {
        const int high = hex_nibble(text[1 + index * 2]);
        const int low = hex_nibble(text[2 + index * 2]);
        if (high < 0 || low < 0) return false;
        channels[index] = static_cast<uint8_t>((high << 4) | low);
    }
    *color = {channels[0], channels[1], channels[2]};
    return true;
}

static uint8_t scale_channel(uint8_t channel, uint8_t level)
{
    if (channel == 0 || level == 0) return 0;
    const uint8_t scaled = static_cast<uint8_t>((static_cast<uint16_t>(channel) * level) / 255);
    return scaled == 0 ? 1 : scaled;
}

#define I2C_PORT                0
#define I2C_SDA_GPIO            ((gpio_num_t)6)
#define I2C_SCL_GPIO            ((gpio_num_t)7)
#define I2C_TIMEOUT_MS          100
#define I2C_SCAN_START_ADDR     0x03
#define I2C_SCAN_END_ADDR       0x77
#define I2C_SCAN_PERIOD_MS      30000
#define I2C_USE_INTERNAL_PULLUP false
#define STATUS_RGB_GPIO         GPIO_NUM_20

static constexpr uint8_t kChargeStopPercentage = 95;
static constexpr uint8_t kChargeResumePercentage = 20;
static constexpr uint16_t kGentleChargeMilliamps = 512;
static constexpr uint16_t kNormalChargeMilliamps = 2048;
static constexpr char kApSsid[] = "Emmaforo-Setup";
static constexpr char kApPassword[] = "emmaforo";

static const char *TAG = "I2C_SCAN";

static PublishedBattery g_published_battery = {};

PublishedBattery emmaforo_published_battery()
{
    return g_published_battery;
}

void emmaforo_note_wifi_use()
{
}

void emmaforo_release_wifi()
{
}

static void print_banner(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, " ESP32-C6 I2C Scanner");
    ESP_LOGI(TAG, " SDA: GPIO%d", I2C_SDA_GPIO);
    ESP_LOGI(TAG, " SCL: GPIO%d", I2C_SCL_GPIO);
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "");
}

static int probe_bq25896_addresses(i2c_master_bus_handle_t bus_handle)
{
    esp_err_t ret = i2c_master_probe(bus_handle, BQ::kDefaultAddress, I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "BQ25896 found at 0x%02X", BQ::kDefaultAddress);
        return 1;
    }
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No response at 0x%02X", BQ::kDefaultAddress);
        return 0;
    }
    ESP_LOGE(TAG, "Probe error at 0x%02X: %s", BQ::kDefaultAddress, esp_err_to_name(ret));
    return -1;
}

extern "C" void app_main(void)
{
    i2c_master_bus_handle_t bus_handle = NULL;
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if (ret == ESP_OK) ret = nvs_flash_init();
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(ret));
    }

#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 10,
        .light_sleep_enable = true,
    };
    ret = esp_pm_configure(&pm_config);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Power management setup failed: %s", esp_err_to_name(ret));
#endif

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = I2C_USE_INTERNAL_PULLUP,
        },
    };

    ret = i2c_new_master_bus(&bus_config, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus initialization failed: %s", esp_err_to_name(ret));
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    print_banner();

    SerialRgbLed rgb_led(STATUS_RGB_GPIO, 1);
    esp_err_t led_err = rgb_led.begin();
    if (led_err != ESP_OK) {
        ESP_LOGE(TAG, "RGB LED init failed: %s", esp_err_to_name(led_err));
    } else {
        rgb_led.set_all(0, 0, 10);
        rgb_led.show();
        vTaskDelay(pdMS_TO_TICKS(300));
        rgb_led.set_pixel(0, 10, 0, 0);
        rgb_led.show();
    }

    BQ charger(bus_handle);
    ret = charger.BQ_begin();
    const bool charger_ready = ret == ESP_OK;
    bool charging_enabled = charger_ready;
    if (charger_ready) {
        ESP_LOGI(TAG, "BQ25896 driver initialized at 0x%02X", BQ::kDefaultAddress);
    } else {
        ESP_LOGW(TAG, "BQ25896 driver initialization failed: %s", esp_err_to_name(ret));
    }

    firmware_update_load();
    ESP_LOGI(TAG, "Firmware version %s", firmware_version());

    SettingsStore settings_store;
    NetworkSettings network_settings = {};
    ret = settings_store.load(&network_settings);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Network settings load failed: %s", esp_err_to_name(ret));
    }
    if (network_settings.device_name[0] == '\0') {
        std::snprintf(network_settings.device_name, sizeof(network_settings.device_name), "Emmaforo");
        if (settings_store.save(network_settings) != ESP_OK) {
            ESP_LOGW(TAG, "Default board name was not stored");
        }
    }
    if (network_settings.ssid[0] == '\0') {
        ESP_LOGI(TAG, "No Wi-Fi network stored on the board");
    } else {
        ESP_LOGI(TAG, "Wi-Fi network stored on the board: %s", network_settings.ssid);
    }

    WifiManager wifi;
    bool wifi_on = false;
    bool sta_join_started = false;
    int64_t sta_join_started_us = 0;
    bool logged_ble_hold = false;
    bool resting = false;
    bool user_charge_paused = network_settings.charge_paused != 0;
    ESP_LOGI(TAG, "Stored network joins unless a phone holds Bluetooth");
    if (charger_ready) {
        if (charger.BQ_pet_watchdog() != ESP_OK) {
            ESP_LOGW(TAG, "Charger watchdog was not reset");
        }
        if (charger.BQ_disable_watchdog() != ESP_OK) {
            ESP_LOGW(TAG, "Charger watchdog was not disabled");
        }
        const uint16_t charge_ma = network_settings.gentle_charge ? kGentleChargeMilliamps
                                                                  : kNormalChargeMilliamps;
        if (charger.BQ_set_charge_current_ma(charge_ma) != ESP_OK) {
            ESP_LOGW(TAG, "Charge current was not applied");
        }
        if (user_charge_paused && charger.BQ_set_charging_enabled(false) == ESP_OK) {
            charging_enabled = false;
        }
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Network stack init failed: %s", esp_err_to_name(ret));
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop init failed: %s", esp_err_to_name(ret));
    }

    SetupPortal portal;
    ret = portal.begin(charger, rgb_led);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP setup portal failed: %s", esp_err_to_name(ret));
    }

    BluetoothManager bluetooth;
    uint8_t stored_colors[SettingsStore::kLampColorPresetSize] = {};
    if (settings_store.load_colors(stored_colors, sizeof(stored_colors)) != ESP_OK) {
        ESP_LOGW(TAG, "Color preset load failed");
        std::memset(stored_colors, 0, sizeof(stored_colors));
    }
    unsigned stored_color_count = 0;
    for (int index = 0; index < 8; ++index) {
        if ((stored_colors[0] & (1u << index)) != 0) stored_color_count++;
    }
    bluetooth.set_color_preset(stored_colors, sizeof(stored_colors));
    ESP_LOGI(TAG, "Configuration restored: name=%s wifi=%s safe=%u pause=%u current=%s colors=%u",
             network_settings.device_name,
             network_settings.ssid[0] == '\0' ? "none" : "stored",
             network_settings.safe_charging_mode, network_settings.charge_paused,
             network_settings.gentle_charge ? "gentle" : "normal", stored_color_count);
    ret = bluetooth.begin(network_settings.device_name, network_settings.safe_charging_mode != 0);
    const std::string initial_ip = wifi.station_ip();
    bluetooth.set_network_status(network_settings.ssid, wifi.is_connected(), initial_ip.c_str(),
                                 network_settings.password[0] != '\0');
    if (wifi.needs_configuration()) {
        ret = bluetooth.begin(network_settings.device_name, network_settings.safe_charging_mode != 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Bluetooth configuration failed: %s", esp_err_to_name(ret));
        }
    }
    if (ret == ESP_OK) firmware_update_mark_valid();

    SerialRgbLed::Color led_color = {0, 0, 0};
    bool led_blink = false;
    bool led_on = false;
    bool led_phase = true;
    uint32_t led_blink_interval_ms = 1000;
    uint8_t led_level = 255;
    bool configuration_led_mode = false;
    Bq25896EstimateSource battery_gauge;
    BatteryStatusSource &battery = battery_gauge;

    while (1) {
        char configured_ssid[64] = {};
        char configured_password[64] = {};
        const bool bluetooth_configured = bluetooth.take_credentials(
            configured_ssid, sizeof(configured_ssid), configured_password, sizeof(configured_password));
        const bool portal_configured = portal.take_credentials(
            configured_ssid, sizeof(configured_ssid), configured_password, sizeof(configured_password));
        if (bluetooth_configured || portal_configured) {
            NetworkSettings new_settings = network_settings;
            std::snprintf(new_settings.ssid, sizeof(new_settings.ssid), "%s", configured_ssid);
            std::snprintf(new_settings.password, sizeof(new_settings.password), "%s", configured_password);
            const esp_err_t save_ret = configured_ssid[0] == '\0'
                ? settings_store.clear_network()
                : settings_store.save(new_settings);
            if (save_ret != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi network was not stored: %s", esp_err_to_name(save_ret));
            } else {
                network_settings = new_settings;
                sta_join_started = false;
                if (wifi_on) {
                    wifi.stop();
                    wifi_on = false;
                }
                if (configured_ssid[0] == '\0') {
                    ESP_LOGI(TAG, "Wi-Fi network erased from the board");
                } else {
                    ESP_LOGI(TAG, "Wi-Fi network stored on the board");
                    if (portal_configured) emmaforo_note_wifi_use();
                }
            }
        }

        if (bluetooth.take_scan_request()) {
            if (!wifi_on) {
                const esp_err_t scan_radio = wifi.prepare_scan();
                if (scan_radio == ESP_OK) {
                    wifi_on = true;
                } else {
                    ESP_LOGW(TAG, "Wi-Fi scan radio failed: %s", esp_err_to_name(scan_radio));
                }
            }
            bluetooth.scan_networks();
        }

        char approved_firmware[32] = {};
        if (bluetooth.take_firmware_approval(approved_firmware, sizeof(approved_firmware))) {
            if (firmware_update_approve(approved_firmware) != ESP_OK) {
                ESP_LOGW(TAG, "Firmware approval was not stored");
            }
        }

        char updated_device_name[sizeof(network_settings.device_name)] = {};
        if (bluetooth.take_device_name(updated_device_name, sizeof(updated_device_name))) {
            std::snprintf(network_settings.device_name, sizeof(network_settings.device_name), "%s",
                          updated_device_name);
            if (settings_store.save(network_settings) != ESP_OK) {
                ESP_LOGE(TAG, "Could not save device name");
            }
        }

        bool requested_safe_mode = network_settings.safe_charging_mode != 0;
        if (bluetooth.take_safe_charging_mode(&requested_safe_mode)) {
            network_settings.safe_charging_mode = requested_safe_mode ? 1 : 0;
            if (settings_store.save(network_settings) != ESP_OK) {
                ESP_LOGE(TAG, "Could not save safe charging mode");
            }
        }

        bool requested_configuration_mode = false;
        if (bluetooth.take_led_mode_command(&requested_configuration_mode)) {
            configuration_led_mode = requested_configuration_mode;
        }

        bool pause_changed = false;
        bool pause_value = false;
        if (portal.take_charging_command(&pause_value)) {
            pause_changed = true;
            user_charge_paused = !pause_value;
        }
        bool ble_pause = false;
        if (bluetooth.take_charge_pause(&ble_pause)) {
            pause_changed = true;
            user_charge_paused = ble_pause;
        }
        if (pause_changed) {
            network_settings.charge_paused = user_charge_paused ? 1 : 0;
            if (settings_store.save(network_settings) != ESP_OK) {
                ESP_LOGE(TAG, "Could not save charge pause");
            }
            if (charger_ready) {
                const bool enable = !user_charge_paused;
                if (charger.BQ_set_charging_enabled(enable) == ESP_OK) {
                    charging_enabled = enable;
                    ESP_LOGI(TAG, "Charging manually %s", enable ? "resumed" : "paused");
                }
            }
        }

        bool gentle_charge = false;
        if (bluetooth.take_gentle_charge(&gentle_charge)) {
            network_settings.gentle_charge = gentle_charge ? 1 : 0;
            if (settings_store.save(network_settings) != ESP_OK) {
                ESP_LOGE(TAG, "Could not save charge current");
            }
            if (charger_ready) {
                const uint16_t charge_ma = gentle_charge ? kGentleChargeMilliamps : kNormalChargeMilliamps;
                if (charger.BQ_set_charge_current_ma(charge_ma) != ESP_OK) {
                    ESP_LOGW(TAG, "Charge current was not applied");
                }
            }
        }

        uint8_t updated_colors[SettingsStore::kLampColorPresetSize] = {};
        if (bluetooth.take_color_preset(updated_colors, sizeof(updated_colors))) {
            if (settings_store.save_colors(updated_colors, sizeof(updated_colors)) != ESP_OK) {
                ESP_LOGE(TAG, "Could not save color preset");
            } else {
                ESP_LOGI(TAG, "Color preset stored on the board");
            }
        }

        if (portal.take_shutdown_command() || bluetooth.take_shutdown()) {
            resting = true;
            led_on = false;
            led_blink = false;
            led_color = {};
            if (wifi_on) {
                wifi.stop();
                wifi_on = false;
                sta_join_started = false;
            }
#if CONFIG_PM_ENABLE
            esp_pm_config_t rest_pm = {
                .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                .min_freq_mhz = 10,
                .light_sleep_enable = true,
            };
            if (esp_pm_configure(&rest_pm) != ESP_OK) {
                ESP_LOGE(TAG, "Light sleep was not enabled");
            }
#endif
            ESP_LOGI(TAG, "Rest: Wi-Fi off, light sleep with BLE modem sleep");
        }

        SerialRgbLed::Color requested_color = {};
        uint8_t requested_led = 255;
        bool requested_blink = false;
        uint32_t requested_blink_interval_ms = led_blink_interval_ms;
        uint8_t requested_level = led_level;
        if (portal.take_led_command(&requested_color, &requested_led, &requested_blink,
                                    &requested_blink_interval_ms, &requested_level)) {
            led_blink_interval_ms = requested_blink_interval_ms;
            led_level = requested_level;
            if (requested_led == 255 || requested_led == 0) {
                led_color = requested_color;
                led_blink = requested_blink;
                led_on = requested_color.r != 0 || requested_color.g != 0 || requested_color.b != 0;
            }
            led_phase = true;
        }

        char ble_led_command[32] = {};
        if (bluetooth.take_led_command(ble_led_command, sizeof(ble_led_command))) {
            resting = false;
            char fields[5][16] = {};
            int field_count = 0;
            size_t field_len = 0;
            for (const char *cursor = ble_led_command; field_count < 5; ++cursor) {
                const bool done = *cursor == '\0';
                if (!done && *cursor != ',' && field_len + 1 < sizeof(fields[field_count])) {
                    fields[field_count][field_len++] = *cursor;
                    continue;
                }
                fields[field_count][field_len] = '\0';
                field_count += 1;
                field_len = 0;
                if (done) break;
            }
            if (field_count >= 3) {
                const unsigned target = static_cast<unsigned>(std::strtoul(fields[0], nullptr, 10));
                const unsigned blink = static_cast<unsigned>(std::strtoul(fields[2], nullptr, 10));
                if (field_count >= 4) {
                    const unsigned interval_ms = static_cast<unsigned>(std::strtoul(fields[3], nullptr, 10));
                    led_blink_interval_ms = interval_ms < 500 ? 500 : interval_ms > 2500 ? 2500 : interval_ms;
                }
                if (field_count >= 5) {
                    const unsigned level = static_cast<unsigned>(std::strtoul(fields[4], nullptr, 10));
                    led_level = level > 255 ? 255 : static_cast<uint8_t>(level);
                }
                SerialRgbLed::Color selected = {};
                if ((target == 255 || target == 0) && parse_lamp_color(fields[1], &selected)) {
                    led_color = selected;
                    led_blink = blink != 0;
                    led_on = selected.r != 0 || selected.g != 0 || selected.b != 0;
                    led_phase = true;
                }
            }
        }

        const bool ble_connected = bluetooth.is_connected();
        const bool ble_held = bluetooth.link_is_held();
        const int64_t now_us = esp_timer_get_time();
        const bool want_wifi = !resting && network_settings.ssid[0] != '\0' && !ble_held;
        if (ble_held) {
            if (!logged_ble_hold) {
                ESP_LOGI(TAG, "Bluetooth held: stored network stays down");
                logged_ble_hold = true;
            }
        } else {
            logged_ble_hold = false;
        }
        const bool wifi_associated = wifi_on && wifi.is_connected();
        if (want_wifi && !wifi_associated) {
            const bool retry = sta_join_started && now_us - sta_join_started_us >= 12LL * 1000000LL;
            if (!sta_join_started || retry) {
                if (retry) {
                    ESP_LOGI(TAG, "Stored network not joined, retrying %s", network_settings.ssid);
                } else {
                    ESP_LOGI(TAG, "No Bluetooth session: joining stored network %s", network_settings.ssid);
                }
                const esp_err_t wifi_ret = wifi.begin_sta(network_settings.ssid, network_settings.password);
                if (wifi_ret == ESP_OK) {
                    wifi_on = true;
                    sta_join_started = true;
                    sta_join_started_us = now_us;
                } else {
                    sta_join_started = false;
                    ESP_LOGW(TAG, "Wi-Fi did not start: %s", esp_err_to_name(wifi_ret));
                }
            }
        } else if (!want_wifi && wifi_on && !firmware_update_busy()) {
            const std::string learned_ip = wifi.station_ip();
            bluetooth.set_network_status(network_settings.ssid, wifi.is_connected(), learned_ip.c_str(),
                                         network_settings.password[0] != '\0');
            wifi.stop();
            wifi_on = false;
            sta_join_started = false;
            ESP_LOGI(TAG, "Bluetooth held: Wi-Fi off");
        }
        const bool wifi_connected = wifi_on && wifi.is_connected();
        firmware_update_poll(wifi_connected);
        const std::string station_ip = wifi.station_ip();
        bluetooth.set_network_status(network_settings.ssid, wifi_connected, station_ip.c_str(),
                                     network_settings.password[0] != '\0');
        if (configuration_led_mode) {
            led_on = true;
            led_blink = !wifi_connected;
            led_color = !wifi_connected ? SerialRgbLed::Color{255, 0, 0}
                : (ble_connected ? SerialRgbLed::Color{0, 255, 0} : SerialRgbLed::Color{255, 180, 0});
        }

        if (led_on && (!led_blink || led_phase)) {
            rgb_led.set_pixel(0, scale_channel(led_color.r, led_level),
                              scale_channel(led_color.g, led_level),
                              scale_channel(led_color.b, led_level));
        } else {
            rgb_led.set_pixel(0, 0, 0, 0);
        }
        rgb_led.show();
        if (led_on && led_blink) led_phase = !led_phase;

        const bool safe_mode = network_settings.safe_charging_mode != 0;
        BatteryStatus status = battery.read(charger, led_on, safe_mode);
        uint8_t fault = 0;
        if (charger.BQ_get_fault(&fault) == ESP_OK) status.fault = fault;

        if (charger_ready && safe_mode && !user_charge_paused && status.valid && charging_enabled &&
            status.percent >= kChargeStopPercentage) {
            if (charger.BQ_set_charging_enabled(false) == ESP_OK) {
                charging_enabled = false;
                battery.anchor(kChargeStopPercentage);
                status.percent = kChargeStopPercentage;
                status.charging = false;
                ESP_LOGI(TAG, "Battery at %u%%: charging paused", status.percent);
            }
        } else if (charger_ready && safe_mode && !user_charge_paused && status.valid && !charging_enabled &&
                   status.charge_state != static_cast<uint8_t>(BQ::ChargeStatus::ChargeDone) &&
                   status.percent <= kChargeResumePercentage) {
            if (charger.BQ_set_charging_enabled(true) == ESP_OK) {
                charging_enabled = true;
                ESP_LOGI(TAG, "Battery at %u%%: charging resumed", status.percent);
            }
        }

        g_published_battery.percentage = status.percent;
        g_published_battery.voltage_mv = status.voltage_mv;
        g_published_battery.charging = status.charging;
        g_published_battery.charge_status = status.charge_state;
        g_published_battery.fault = status.fault;
        g_published_battery.valid = status.valid;
        if (status.voltage_mv != 0 || status.charge_state != 255) {
            bluetooth.set_battery_measurement(status.percent, status.voltage_mv, status.charging,
                                              status.charge_state, status.fault, safe_mode,
                                              user_charge_paused, network_settings.gentle_charge != 0,
                                              status.valid);
        }

        if (status.voltage_mv != 0 || status.charge_state != 255) {
            ESP_LOGI(TAG, "Battery status: %u%%, %u mV, state=%u", status.percent,
                     status.voltage_mv, status.charge_state);
        } else {
            ESP_LOGW(TAG, "Battery status unavailable");
        }

        int found = probe_bq25896_addresses(bus_handle);

        if (found < 0) {
            ESP_LOGE(TAG, "I2C bus error detected: the bus is not healthy.");
        } else if (found == 0) {
            ESP_LOGW(TAG, "BQ25896 not found at 0x6A or 0x6B.");
        } else {
            ESP_LOGI(TAG, "BQ25896 detected on the I2C bus.");
        }

        if (resting) {
            if (charger_ready && charger.BQ_disable_watchdog() != ESP_OK) {
                ESP_LOGW(TAG, "Charger watchdog stayed armed during light sleep");
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            if (charger_ready && charger.BQ_pet_watchdog() != ESP_OK) {
                ESP_LOGW(TAG, "Charger watchdog was not reset");
            }
            uint32_t loop_delay_ms = I2C_SCAN_PERIOD_MS;
            if (firmware_update_pending() ||
                (!ble_held && !wifi.is_connected() && network_settings.ssid[0] != '\0')) {
                loop_delay_ms = 1000;
            } else if (led_on && led_blink && led_blink_interval_ms < I2C_SCAN_PERIOD_MS) {
                loop_delay_ms = led_blink_interval_ms;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(loop_delay_ms));
        }
    }
}
