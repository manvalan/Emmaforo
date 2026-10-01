 #include <stdio.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_PORT            0
#define I2C_SDA_GPIO        ((gpio_num_t)6)
#define I2C_SCL_GPIO        ((gpio_num_t)7)
#define I2C_TIMEOUT_MS      100
#define I2C_SCAN_START_ADDR 0x03
#define I2C_SCAN_END_ADDR   0x77
#define I2C_SCAN_PERIOD_MS  2000

static const char *TAG = "I2C_SCAN";

extern "C" void app_main(void)
{
    i2c_master_bus_handle_t bus_handle = NULL;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = false,
        },
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, " ESP32-C6 I2C Scanner");
    ESP_LOGI(TAG, " SDA: GPIO%d", I2C_SDA_GPIO);
    ESP_LOGI(TAG, " SCL: GPIO%d", I2C_SCL_GPIO);
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "");

    while (1) {
        int found = 0;

        ESP_LOGI(TAG, "Scanning...");

        for (uint8_t addr = I2C_SCAN_START_ADDR; addr <= I2C_SCAN_END_ADDR; addr++) {
            esp_err_t ret = i2c_master_probe(bus_handle, addr, I2C_TIMEOUT_MS);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Device found at 0x%02X", addr);
                found++;
            }
        }

        if (found == 0) {
            ESP_LOGW(TAG, "No I2C devices found.");
        } else {
            ESP_LOGI(TAG, "Found %d device(s).", found);
        }

        vTaskDelay(pdMS_TO_TICKS(I2C_SCAN_PERIOD_MS));
    }
}