#pragma once

#include <stdint.h>
#include <vector>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"

class SerialRgbLed {
public:
    struct Color {
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    SerialRgbLed(gpio_num_t data_pin, uint8_t led_count = 1);
    ~SerialRgbLed();

    esp_err_t begin();
    void set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
    void set_pixel(uint8_t index, Color color);
    void set_all(uint8_t r, uint8_t g, uint8_t b);
    void off();
    void blink(uint8_t r, uint8_t g, uint8_t b, uint32_t on_ms = 200, uint32_t off_ms = 200, uint8_t repeats = 1);
    void show();

private:
    gpio_num_t data_pin_;
    uint8_t led_count_;
    std::vector<Color> colors_;
    rmt_channel_handle_t tx_channel_;
    rmt_encoder_handle_t bytes_encoder_;
};
