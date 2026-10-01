#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

class BluetoothManager {
public:
    BluetoothManager();
    ~BluetoothManager();

    esp_err_t begin(const char *device_name = nullptr, bool safe_charging_mode = true);
    esp_err_t stop();
    bool is_enabled() const;
    bool is_connected() const;
    bool link_is_held();
    bool take_scan_request();
    void scan_networks();
    void set_battery_measurement(uint8_t percentage, uint16_t millivolts, bool charging,
                                 uint8_t charge_status, uint8_t fault, bool safe_charging,
                                 bool charge_paused, bool gentle_charge, bool estimate_valid,
                                 bool power_good);
    bool take_charge_pause(bool *paused);
    bool take_gentle_charge(bool *gentle);
    bool take_shutdown();
    bool take_device_name(char *name, size_t name_size);
    bool take_safe_charging_mode(bool *enabled);
    bool take_led_command(char *command, size_t command_size);
    bool take_led_mode_command(bool *configuration_mode);
    void set_network_status(const char *ssid, bool connected, const char *ip_address, bool password_stored);
    void set_color_preset(const uint8_t *data, size_t size);
    bool take_color_preset(uint8_t *data, size_t size);
    bool take_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size);
    bool take_firmware_approval(char *version, size_t version_size);

private:
    bool initialized_;
};
