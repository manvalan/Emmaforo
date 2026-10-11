#include <stdio.h>
#include <stdint.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include "driver/i2c_master.h"
#include "bq25896.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "battery_log.h"
#include "battery_policy.h"
#include "battery_status.h"
#include "bluetooth_manager.h"
#include "firmware_update.h"
#include "max17048.h"
#include "remote_link.h"
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

// Fuel gauge alerts: empty at 10%, cell under 3.40 V or over 4.30 V.
static constexpr uint8_t kGaugeEmptyAlertPercent = 10;
static constexpr uint16_t kGaugeLowAlertMv = 3400;
static constexpr uint16_t kGaugeHighAlertMv = 4300;
// While resting the loop still wakes this often, so safe charging keeps its
// limits when the board sits on a charger.
static constexpr uint32_t kRestLoopMs = 60000;
static constexpr int64_t kBatteryReadMinUs = 5LL * 1000000LL;
static constexpr char kApSsid[] = "Emmaforo-Setup";
static constexpr char kApPassword[] = "emmaforo";

static const char *TAG = "I2C_SCAN";

static PublishedBattery g_published_battery = {};
static PublishedInfo g_published_info = {};
static char g_serial[18] = {};

PublishedBattery emmaforo_published_battery()
{
    return g_published_battery;
}

PublishedInfo emmaforo_published_info()
{
    return g_published_info;
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

struct ChargeWindow {
    bool allow;
    bool climb;
    bool reached_stop;
};

// shown_percent is the percent the user sees (battery_policy::shown_percent).
// Outside safe mode the charger itself stops at the 95% point.
static ChargeWindow safe_charge_window(bool safe_mode, bool user_paused, bool climb,
                                        bool percent_known, uint8_t shown_percent)
{
    ChargeWindow decision = {true, false, false};
    if (user_paused) {
        decision.allow = false;
        decision.climb = climb;
        return decision;
    }
    if (!safe_mode) {
        return decision;
    }
    if (!percent_known) {
        decision.allow = false;
        return decision;
    }
    if (shown_percent >= battery_policy::kSafeStopShownPercent) {
        decision.allow = false;
        decision.reached_stop = true;
        return decision;
    }
    if (shown_percent < battery_policy::kSafeResumeBelowShownPercent || climb) {
        decision.allow = true;
        decision.climb = true;
        return decision;
    }
    decision.allow = false;
    return decision;
}

static uint16_t charge_current_for(bool safe_mode, bool gentle)
{
    if (safe_mode) return battery_policy::kSafeTopUpMilliamps;
    return gentle ? battery_policy::kGentleChargeMilliamps : battery_policy::kNormalChargeMilliamps;
}

static void apply_charge_current(BQ &charger, bool charger_ready, uint16_t milliamps, uint16_t *applied_ma)
{
    if (!charger_ready || applied_ma == nullptr || *applied_ma == milliamps) return;
    if (charger.BQ_set_charge_current_ma(milliamps) == ESP_OK) {
        *applied_ma = milliamps;
    } else {
        ESP_LOGW(TAG, "Charge current was not applied");
    }
}

static void apply_charger_enable(BQ &charger, bool charger_ready, bool allow, bool *charging_enabled)
{
    if (!charger_ready || charging_enabled == nullptr || allow == *charging_enabled) return;
    if (charger.BQ_set_charging_enabled(allow) == ESP_OK) {
        *charging_enabled = allow;
    }
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

    // Boards exist with and without the MAX17048G. Without it, or if it does
    // not answer now, the BQ25896 estimate is used exactly as before.
    Max17048 gauge(bus_handle);
    ret = gauge.begin(kGaugeEmptyAlertPercent, kGaugeLowAlertMv, kGaugeHighAlertMv);
    const bool gauge_ready = ret == ESP_OK;
    if (!gauge_ready) {
        ESP_LOGI(TAG, "No MAX17048 fuel gauge (%s): using the charger estimate", esp_err_to_name(ret));
    }
    Bq25896EstimateSource estimate_source;
    Max17048Source gauge_source(gauge, estimate_source);
    BatteryStatusSource &battery = gauge_ready ? static_cast<BatteryStatusSource &>(gauge_source)
                                               : static_cast<BatteryStatusSource &>(estimate_source);
    battery_log::init();

    firmware_update_load();
    ESP_LOGI(TAG, "Firmware version %s", firmware_version());
    uint8_t factory_mac[6] = {};
    if (esp_efuse_mac_get_default(factory_mac) == ESP_OK) {
        std::snprintf(g_serial, sizeof(g_serial), "%02X:%02X:%02X:%02X:%02X:%02X",
                      factory_mac[0], factory_mac[1], factory_mac[2],
                      factory_mac[3], factory_mac[4], factory_mac[5]);
    }

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
    uint8_t remembered_percent = 0;
    bool remembered_known = false;
    bool charge_climb = false;
    bool saved_climb = false;
    if (settings_store.load_battery_estimate(&remembered_percent, &remembered_known, &charge_climb) != ESP_OK) {
        ESP_LOGW(TAG, "Battery estimate was not restored");
        remembered_known = false;
        charge_climb = false;
    }
    if (remembered_known) {
        estimate_source.restore(remembered_percent);
        saved_climb = charge_climb;
        ESP_LOGI(TAG, "Battery estimate restored at %u%%", remembered_percent);
    } else {
        charge_climb = false;
    }
    uint8_t boot_percent = remembered_percent;
    bool boot_known = remembered_known;
    if (gauge_ready) {
        uint16_t soc = 0;
        if (gauge.read_soc_x256(&soc) == ESP_OK) {
            const uint32_t percent = (static_cast<uint32_t>(soc) + 128U) / 256U;
            boot_percent = static_cast<uint8_t>(percent > 100U ? 100U : percent);
            boot_known = true;
            estimate_source.anchor(boot_percent);
            ESP_LOGI(TAG, "Fuel gauge reports %u%% at boot", boot_percent);
        }
    }
    ESP_LOGI(TAG, "Stored network joins unless a phone holds Bluetooth");
    uint16_t applied_charge_ma = 0;
    if (charger_ready) {
        if (charger.BQ_pet_watchdog() != ESP_OK) {
            ESP_LOGW(TAG, "Charger watchdog was not reset");
        }
        if (charger.BQ_disable_watchdog() != ESP_OK) {
            ESP_LOGW(TAG, "Charger watchdog was not disabled");
        }
        if (charger.BQ_set_charge_voltage_mv(battery_policy::kChargeVoltageMv) != ESP_OK) {
            ESP_LOGW(TAG, "Charge voltage was not applied");
        }
        if (charger.BQ_set_termination_current_ma(battery_policy::kTerminationMilliamps) != ESP_OK) {
            ESP_LOGW(TAG, "Termination current was not applied");
        }
        if (gauge_ready && charger.BQ_set_adc_continuous(false) != ESP_OK) {
            ESP_LOGW(TAG, "Charger ADC stays in continuous mode");
        }
        apply_charge_current(charger, charger_ready,
                             charge_current_for(network_settings.safe_charging_mode != 0,
                                                network_settings.gentle_charge != 0),
                             &applied_charge_ma);
        const ChargeWindow boot_window = safe_charge_window(
            network_settings.safe_charging_mode != 0, user_charge_paused, charge_climb,
            boot_known, battery_policy::shown_percent(boot_percent));
        charge_climb = boot_window.climb;
        apply_charger_enable(charger, charger_ready, boot_window.allow, &charging_enabled);
        if (!boot_window.allow) {
            if (user_charge_paused) {
                ESP_LOGI(TAG, "Charging paused");
            } else if (boot_window.reached_stop) {
                ESP_LOGI(TAG, "Battery at %u%%: safe charge holds", battery_policy::shown_percent(boot_percent));
            } else if (boot_known) {
                ESP_LOGI(TAG, "Battery at %u%%: safe charge stays off", battery_policy::shown_percent(boot_percent));
            } else {
                ESP_LOGI(TAG, "Safe charge stays off until the battery is known below %u%%",
                         battery_policy::kSafeResumeBelowShownPercent);
            }
        } else if (network_settings.safe_charging_mode != 0) {
            ESP_LOGI(TAG, "Battery at %u%%: gentle top-up", battery_policy::shown_percent(boot_percent));
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

    // The local HTTP page has no authentication. Everything it did is now done
    // over Bluetooth, so it stays off unless a build turns it on explicitly.
    SetupPortal portal;
#if CONFIG_EMMAFORO_HTTP_PORTAL
    ret = portal.begin(charger, rgb_led);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP setup portal failed: %s", esp_err_to_name(ret));
    }
#endif

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
    // Bluetooth is the only normal link: a new image is confirmed only once it is up.
    ret = bluetooth.begin(network_settings.device_name, network_settings.safe_charging_mode != 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth configuration failed: %s", esp_err_to_name(ret));
    }
    bluetooth.set_network_status(network_settings.ssid, false, "", network_settings.password[0] != '\0');
    if (ret == ESP_OK) firmware_update_mark_valid();

    RemoteLink remote;
    remote.begin();

    SerialRgbLed::Color led_color = {0, 0, 0};
    bool led_blink = false;
    bool led_on = false;
    bool led_phase = true;
    uint32_t led_blink_interval_ms = 1000;
    uint8_t led_level = 255;
    bool configuration_led_mode = false;
    BatteryStatus status = {};
    bool have_status = false;
    bool was_resting = false;
    int64_t last_battery_read_us = 0;

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

        char remote_setting[200] = {};
        if (bluetooth.take_remote_command(remote_setting, sizeof(remote_setting)) &&
            !remote.apply_command(remote_setting)) {
            ESP_LOGW(TAG, "Remote setting not understood");
        }
        // Commands from the broker use the same text as the Bluetooth ones.
        bool remote_pause_set = false;
        bool remote_pause = false;
        bool remote_gentle_set = false;
        bool remote_gentle = false;
        bool remote_rest = false;
        char remote_led[32] = {};
        char remote_command[64] = {};
        while (remote.take_command(remote_command, sizeof(remote_command))) {
            ESP_LOGI(TAG, "Remote command: %s", remote_command);
            if (std::strcmp(remote_command, "pause:1") == 0 || std::strcmp(remote_command, "pause:0") == 0) {
                remote_pause_set = true;
                remote_pause = remote_command[6] == '1';
            } else if (std::strcmp(remote_command, "gentle:1") == 0 || std::strcmp(remote_command, "gentle:0") == 0) {
                remote_gentle_set = true;
                remote_gentle = remote_command[7] == '1';
            } else if (std::strcmp(remote_command, "riposo") == 0) {
                remote_rest = true;
            } else if (remote_command[0] >= '0' && remote_command[0] <= '9' &&
                       std::strlen(remote_command) < sizeof(remote_led)) {
                std::memcpy(remote_led, remote_command, std::strlen(remote_command) + 1);
            }
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
        if (remote_pause_set) {
            pause_changed = true;
            user_charge_paused = remote_pause;
        }
        if (pause_changed) {
            network_settings.charge_paused = user_charge_paused ? 1 : 0;
            if (settings_store.save(network_settings) != ESP_OK) {
                ESP_LOGE(TAG, "Could not save charge pause");
            }
            ESP_LOGI(TAG, "Charging manually %s", user_charge_paused ? "paused" : "resumed");
        }

        bool gentle_charge = network_settings.gentle_charge != 0;
        bool current_changed = false;
        if (bluetooth.take_gentle_charge(&gentle_charge)) current_changed = true;
        if (remote_gentle_set) {
            gentle_charge = remote_gentle;
            current_changed = true;
        }
        bool http_gentle = false;
        if (portal.take_charge_current(&http_gentle)) {
            gentle_charge = http_gentle;
            current_changed = true;
        }
        if (current_changed) {
            network_settings.gentle_charge = gentle_charge ? 1 : 0;
            if (settings_store.save(network_settings) != ESP_OK) {
                ESP_LOGE(TAG, "Could not save charge current");
            }
        }

        uint8_t updated_colors[SettingsStore::kLampColorPresetSize] = {};
        if (bluetooth.take_color_preset(updated_colors, sizeof(updated_colors))) {
            if (settings_store.save_colors(updated_colors, sizeof(updated_colors)) != ESP_OK) {
                ESP_LOGE(TAG, "Could not save color preset");
            } else {
                std::memcpy(stored_colors, updated_colors, sizeof(stored_colors));
                ESP_LOGI(TAG, "Color preset stored on the board");
            }
        }

        if (portal.take_shutdown_command() || bluetooth.take_shutdown() || remote_rest) {
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
        bool lamp_command = bluetooth.take_led_command(ble_led_command, sizeof(ble_led_command));
        if (!lamp_command && remote_led[0] != '\0') {
            std::snprintf(ble_led_command, sizeof(ble_led_command), "%s", remote_led);
            lamp_command = true;
        }
        if (lamp_command) {
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
        // Wi-Fi is off by default. It joins the stored network only to fetch a
        // firmware the user approved, and only for as long as that takes.
        const bool update_wanted = firmware_update_pending();
        // Remote control (off unless the user turned it on) decides by itself
        // between staying associated on USB, TWT on a Wi-Fi 6 router, or
        // short check-ins. It keeps working while resting so the lamp can be
        // woken from afar.
        remote.poll(have_status && status.power_good, wifi_on && wifi.is_connected(), now_us);
        const bool want_wifi = network_settings.ssid[0] != '\0' &&
                               ((update_wanted && !resting) || remote.wants_wifi());
        if (ble_held) {
            if (!logged_ble_hold) {
                ESP_LOGI(TAG, "Bluetooth session held");
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
                    ESP_LOGI(TAG, "Firmware update approved: joining stored network %s", network_settings.ssid);
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
            wifi.stop();
            wifi_on = false;
            sta_join_started = false;
            ESP_LOGI(TAG, "Nothing needs Wi-Fi: Wi-Fi off");
        }
        const bool wifi_connected = wifi_on && wifi.is_connected();
        firmware_update_poll(wifi_connected);
        const std::string station_ip = wifi.station_ip();
        bluetooth.set_network_status(network_settings.ssid, wifi_connected, station_ip.c_str(),
                                     network_settings.password[0] != '\0');
        if (configuration_led_mode) {
            // Bluetooth is the link: green when a phone is connected, amber
            // blinking while waiting for one, red blinking while an approved
            // firmware is being fetched.
            led_on = true;
            if (update_wanted) {
                led_blink = true;
                led_color = SerialRgbLed::Color{255, 0, 0};
            } else {
                led_blink = !ble_connected;
                led_color = ble_connected ? SerialRgbLed::Color{0, 255, 0} : SerialRgbLed::Color{255, 180, 0};
            }
        }

        const SerialRgbLed::Color shown_led = {scale_channel(led_color.r, led_level),
                                               scale_channel(led_color.g, led_level),
                                               scale_channel(led_color.b, led_level)};
        if (led_on && (!led_blink || led_phase)) {
            rgb_led.set_pixel(0, shown_led.r, shown_led.g, shown_led.b);
        } else {
            rgb_led.set_pixel(0, 0, 0, 0);
        }
        rgb_led.show();
        if (led_on && led_blink) led_phase = !led_phase;

        const bool safe_mode = network_settings.safe_charging_mode != 0;
        // A blinking lamp runs this loop up to twice a second. The battery
        // does not change that fast, so the I2C reads are spaced out.
        if (!have_status || now_us - last_battery_read_us >= kBatteryReadMinUs || was_resting != resting) {
            status = battery.read(charger, led_on, safe_mode);
            uint8_t fault = 0;
            if (charger.BQ_get_fault(&fault) == ESP_OK) status.fault = fault;
            last_battery_read_us = now_us;
            have_status = true;
        }
        was_resting = resting;

        apply_charge_current(charger, charger_ready,
                             charge_current_for(safe_mode, network_settings.gentle_charge != 0),
                             &applied_charge_ma);
        const uint8_t shown = battery_policy::shown_percent(status.percent);
        const ChargeWindow window = safe_charge_window(safe_mode, user_charge_paused, charge_climb,
                                                       status.valid, shown);
        const bool was_charging = charging_enabled;
        charge_climb = window.climb;
        apply_charger_enable(charger, charger_ready, window.allow, &charging_enabled);
        if (!charging_enabled) status.charging = false;
        if (was_charging != charging_enabled) {
            if (charging_enabled && status.valid) {
                ESP_LOGI(TAG, "Battery at %u%%: charging resumed", shown);
            } else if (user_charge_paused) {
                ESP_LOGI(TAG, "Charging paused");
            } else if (window.reached_stop) {
                ESP_LOGI(TAG, "Battery at %u%%: safe charge holds", shown);
            } else if (status.valid) {
                ESP_LOGI(TAG, "Battery at %u%%: safe charge stays off", shown);
            } else {
                ESP_LOGI(TAG, "Safe charge stays off until the battery is known below %u%%",
                         battery_policy::kSafeResumeBelowShownPercent);
            }
        }
        uint8_t live_percent = 0;
        const bool live_known = battery.estimate(&live_percent);
        if (live_known != remembered_known || (live_known && live_percent != remembered_percent) ||
            charge_climb != saved_climb) {
            if (settings_store.save_battery_estimate(live_known, live_known ? live_percent : 0, charge_climb) ==
                ESP_OK) {
                remembered_known = live_known;
                if (live_known) remembered_percent = live_percent;
                saved_climb = charge_climb;
            } else {
                ESP_LOGW(TAG, "Battery estimate was not stored");
            }
        }

        BatteryLogContext log_context = {};
        log_context.lamp_on = led_on;
        log_context.led_load = static_cast<uint8_t>((shown_led.r + shown_led.g + shown_led.b) / 3);
        log_context.wifi_on = wifi_on;
        log_context.ble_connected = ble_connected;
        log_context.resting = resting;
        battery_log::record(status, log_context);

        if (remote.enabled()) {
            char state_json[192] = {};
            std::snprintf(state_json, sizeof(state_json),
                          "{\"pct\":%u,\"valid\":%u,\"charging\":%u,\"usb\":%u,\"on\":%u,"
                          "\"color\":\"#%02X%02X%02X\",\"blink\":%u,\"level\":%u,\"rest\":%u,"
                          "\"mode\":%u,\"interval\":%u,\"fw\":\"%s\"}",
                          shown, status.valid ? 1 : 0, status.charging ? 1 : 0, status.power_good ? 1 : 0,
                          led_on ? 1 : 0, led_color.r, led_color.g, led_color.b, led_blink ? 1 : 0, led_level,
                          resting ? 1 : 0, static_cast<unsigned>(remote.mode()), remote.interval_minutes(),
                          firmware_version());
            remote.publish_state(state_json);
        }
        char remote_status_text[96] = {};
        remote.status_text(remote_status_text, sizeof(remote_status_text));
        bluetooth.set_remote_status(remote_status_text);

        // Everything published uses the shown scale: the 95% point reads 100%.
        g_published_battery.percentage = shown;
        g_published_battery.voltage_mv = status.voltage_mv;
        g_published_battery.charging = status.charging;
        g_published_battery.charge_status = status.charge_state;
        g_published_battery.fault = status.fault;
        g_published_battery.valid = status.valid;
        g_published_battery.power_good = status.power_good;
        std::snprintf(g_published_info.name, sizeof(g_published_info.name), "%s", network_settings.device_name);
        std::snprintf(g_published_info.wifi, sizeof(g_published_info.wifi), "%s", network_settings.ssid);
        g_published_info.wifi_saved = network_settings.ssid[0] != '\0';
        g_published_info.password_saved = network_settings.password[0] != '\0';
        g_published_info.safe = network_settings.safe_charging_mode != 0;
        g_published_info.paused = user_charge_paused;
        g_published_info.gentle = network_settings.gentle_charge != 0;
        unsigned published_colors = 0;
        for (int index = 0; index < 8; ++index) {
            if ((stored_colors[0] & (1u << index)) != 0) published_colors++;
        }
        g_published_info.colors = static_cast<uint8_t>(published_colors);
        std::snprintf(g_published_info.serial, sizeof(g_published_info.serial), "%s", g_serial);
        std::snprintf(g_published_info.firmware, sizeof(g_published_info.firmware), "%s", firmware_version());
        std::memcpy(g_published_info.preset, stored_colors, sizeof(g_published_info.preset));
        if (status.voltage_mv != 0 || status.charge_state != 255) {
            bluetooth.set_battery_measurement(shown, status.voltage_mv, status.charging,
                                              status.charge_state, status.fault, safe_mode,
                                              user_charge_paused, network_settings.gentle_charge != 0,
                                              status.valid, status.power_good, status.gauge,
                                              status.rate_centi_per_hour, status.gauge_alerts);
        }

        if (status.voltage_mv != 0 || status.charge_state != 255) {
            ESP_LOGI(TAG, "Battery status: %u%% shown (%u.%u%% real%s), %u mV, state=%u", shown,
                     status.soc_x10 / 10, status.soc_x10 % 10, status.gauge ? ", gauge" : "",
                     status.voltage_mv, status.charge_state);
        } else {
            ESP_LOGW(TAG, "Battery status unavailable");
        }

        int found = probe_bq25896_addresses(bus_handle);

        if (found < 0) {
            ESP_LOGE(TAG, "I2C bus error detected: the bus is not healthy.");
        } else if (found == 0) {
            ESP_LOGW(TAG, "BQ25896 not found at 0x%02X.", BQ::kDefaultAddress);
        } else {
            ESP_LOGI(TAG, "BQ25896 detected on the I2C bus.");
        }

        if (resting) {
            if (charger_ready && charger.BQ_disable_watchdog() != ESP_OK) {
                ESP_LOGW(TAG, "Charger watchdog stayed armed during light sleep");
            }
            uint32_t rest_delay_ms = kRestLoopMs;
            if (remote.session_active() || (remote.wants_wifi() && !wifi.is_connected())) rest_delay_ms = 1000;
            rest_delay_ms = std::min(rest_delay_ms, remote.next_wake_ms(esp_timer_get_time()));
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(rest_delay_ms));
        } else {
            if (charger_ready && charger.BQ_pet_watchdog() != ESP_OK) {
                ESP_LOGW(TAG, "Charger watchdog was not reset");
            }
            uint32_t loop_delay_ms = I2C_SCAN_PERIOD_MS;
            if ((want_wifi && !wifi.is_connected()) || update_wanted || firmware_update_busy()) {
                loop_delay_ms = 1000;
            } else if (led_on && led_blink && led_blink_interval_ms < I2C_SCAN_PERIOD_MS) {
                loop_delay_ms = led_blink_interval_ms;
            }
            if (remote.session_active()) loop_delay_ms = std::min<uint32_t>(loop_delay_ms, 1000);
            loop_delay_ms = std::min(loop_delay_ms, remote.next_wake_ms(esp_timer_get_time()));
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(loop_delay_ms));
        }
    }
}
