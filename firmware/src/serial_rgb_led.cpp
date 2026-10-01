#include "serial_rgb_led.h"

#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
struct Ws2812Encoder {
    rmt_encoder_t base;
    rmt_encoder_handle_t bytes_encoder;
    rmt_encoder_handle_t copy_encoder;
    uint8_t state;
    rmt_symbol_word_t reset_code;
};

size_t encode_ws2812(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                     const void *data, size_t data_size, rmt_encode_state_t *result)
{
    auto *ws_encoder = reinterpret_cast<Ws2812Encoder *>(encoder);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded = 0;

    if (ws_encoder->state == 0) {
        encoded += ws_encoder->bytes_encoder->encode(
            ws_encoder->bytes_encoder, channel, data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state = static_cast<rmt_encode_state_t>(state | RMT_ENCODING_MEM_FULL);
            *result = state;
            return encoded;
        }
    }

    encoded += ws_encoder->copy_encoder->encode(
        ws_encoder->copy_encoder, channel, &ws_encoder->reset_code,
        sizeof(ws_encoder->reset_code), &session_state);
    if (session_state & RMT_ENCODING_COMPLETE) {
        ws_encoder->state = 0;
        state = static_cast<rmt_encode_state_t>(state | RMT_ENCODING_COMPLETE);
    }
    if (session_state & RMT_ENCODING_MEM_FULL) {
        state = static_cast<rmt_encode_state_t>(state | RMT_ENCODING_MEM_FULL);
    }
    *result = state;
    return encoded;
}

esp_err_t delete_ws2812(rmt_encoder_t *encoder)
{
    auto *ws_encoder = reinterpret_cast<Ws2812Encoder *>(encoder);
    rmt_del_encoder(ws_encoder->bytes_encoder);
    rmt_del_encoder(ws_encoder->copy_encoder);
    std::free(ws_encoder);
    return ESP_OK;
}

esp_err_t reset_ws2812(rmt_encoder_t *encoder)
{
    auto *ws_encoder = reinterpret_cast<Ws2812Encoder *>(encoder);
    rmt_encoder_reset(ws_encoder->bytes_encoder);
    rmt_encoder_reset(ws_encoder->copy_encoder);
    ws_encoder->state = 0;
    return ESP_OK;
}

esp_err_t create_ws2812_encoder(rmt_encoder_handle_t *result)
{
    auto *ws_encoder = static_cast<Ws2812Encoder *>(std::calloc(1, sizeof(Ws2812Encoder)));
    if (ws_encoder == nullptr) return ESP_ERR_NO_MEM;

    ws_encoder->base.encode = encode_ws2812;
    ws_encoder->base.del = delete_ws2812;
    ws_encoder->base.reset = reset_ws2812;

    rmt_bytes_encoder_config_t bytes_config = {};
    bytes_config.bit0.duration0 = 3;
    bytes_config.bit0.level0 = 1;
    bytes_config.bit0.duration1 = 9;
    bytes_config.bit0.level1 = 0;
    bytes_config.bit1.duration0 = 9;
    bytes_config.bit1.level0 = 1;
    bytes_config.bit1.duration1 = 3;
    bytes_config.bit1.level1 = 0;
    bytes_config.flags.msb_first = 1;

    esp_err_t ret = rmt_new_bytes_encoder(&bytes_config, &ws_encoder->bytes_encoder);
    if (ret != ESP_OK) {
        std::free(ws_encoder);
        return ret;
    }
    rmt_copy_encoder_config_t copy_config = {};
    ret = rmt_new_copy_encoder(&copy_config, &ws_encoder->copy_encoder);
    if (ret != ESP_OK) {
        rmt_del_encoder(ws_encoder->bytes_encoder);
        std::free(ws_encoder);
        return ret;
    }

    ws_encoder->reset_code.duration0 = 250;
    ws_encoder->reset_code.level0 = 0;
    ws_encoder->reset_code.duration1 = 250;
    ws_encoder->reset_code.level1 = 0;
    *result = &ws_encoder->base;
    return ESP_OK;
}
}

SerialRgbLed::SerialRgbLed(gpio_num_t data_pin, uint8_t led_count)
    : data_pin_(data_pin), led_count_(led_count), colors_(led_count, {0, 0, 0}),
      tx_channel_(nullptr), bytes_encoder_(nullptr)
{
}

SerialRgbLed::~SerialRgbLed()
{
    if (tx_channel_ != nullptr) {
        rmt_disable(tx_channel_);
    }
    if (bytes_encoder_ != nullptr) {
        rmt_del_encoder(bytes_encoder_);
    }
    if (tx_channel_ != nullptr) {
        rmt_del_channel(tx_channel_);
    }
}

esp_err_t SerialRgbLed::begin()
{
    rmt_tx_channel_config_t channel_config = {};
    channel_config.gpio_num = data_pin_;
    channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
    channel_config.resolution_hz = 10000000;
    channel_config.mem_block_symbols = 48;
    channel_config.trans_queue_depth = 1;

    esp_err_t ret = rmt_new_tx_channel(&channel_config, &tx_channel_);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = create_ws2812_encoder(&bytes_encoder_);
    if (ret != ESP_OK) {
        rmt_del_channel(tx_channel_);
        tx_channel_ = nullptr;
        return ret;
    }
    ret = rmt_enable(tx_channel_);
    if (ret != ESP_OK) {
        rmt_del_encoder(bytes_encoder_);
        bytes_encoder_ = nullptr;
        rmt_del_channel(tx_channel_);
        tx_channel_ = nullptr;
    }
    return ret;
}

void SerialRgbLed::set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= led_count_) {
        return;
    }
    colors_[index] = {r, g, b};
}

void SerialRgbLed::set_pixel(uint8_t index, Color color)
{
    set_pixel(index, color.r, color.g, color.b);
}

void SerialRgbLed::set_all(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint8_t i = 0; i < led_count_; ++i) {
        colors_[i] = {r, g, b};
    }
}

void SerialRgbLed::off()
{
    set_all(0, 0, 0);
    show();
}

void SerialRgbLed::blink(uint8_t r, uint8_t g, uint8_t b, uint32_t on_ms, uint32_t off_ms, uint8_t repeats)
{
    for (uint8_t i = 0; i < repeats; ++i) {
        set_all(r, g, b);
        show();
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        off();
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

void SerialRgbLed::show()
{
    if (tx_channel_ == nullptr || bytes_encoder_ == nullptr) {
        return;
    }
    std::vector<uint8_t> payload;
    payload.reserve(static_cast<size_t>(led_count_) * 3);
    for (uint8_t led = 0; led < led_count_; ++led) {
        payload.push_back(colors_[led].g);
        payload.push_back(colors_[led].r);
        payload.push_back(colors_[led].b);
    }

    rmt_transmit_config_t transmit_config = {};
    rmt_transmit(tx_channel_, bytes_encoder_, payload.data(), payload.size(), &transmit_config);
    rmt_tx_wait_all_done(tx_channel_, pdMS_TO_TICKS(100));
}
