#include "color_chain.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "nvs.h"

namespace {
constexpr char kTag[] = "CHAIN";
constexpr char kNamespace[] = "chain";
constexpr char kStepsKey[] = "steps";
constexpr uint8_t kStop = 0x00;
constexpr uint8_t kStore = 0x01;
constexpr uint8_t kRestart = 0x02;
constexpr uint8_t kFlagRepeat = 0x01;
// A fade is redrawn this often: smooth to the eye, light on the battery.
constexpr uint32_t kFrameMs = 20;

uint16_t get_u16(const uint8_t *in)
{
    return static_cast<uint16_t>(in[0] | (in[1] << 8));
}

void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>(value >> 8);
}

uint8_t mix(uint8_t from, uint8_t to, int64_t done, int64_t total)
{
    return static_cast<uint8_t>(from + (static_cast<int32_t>(to) - from) * done / total);
}
}

void ColorChain::load()
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t blob[kMaxBytes] = {};
    size_t length = sizeof(blob);
    if (nvs_get_blob(handle, kStepsKey, blob, &length) == ESP_OK && length >= 2) {
        const size_t count = blob[1];
        if (count <= kMaxSteps && length == 2 + count * kStepBytes) {
            repeat_ = (blob[0] & kFlagRepeat) != 0;
            count_ = count;
            for (size_t index = 0; index < count; ++index) {
                const uint8_t *in = blob + 2 + index * kStepBytes;
                steps_[index] = {{in[0], in[1], in[2]}, get_u16(in + 3), get_u16(in + 5), in[7]};
            }
        }
    }
    nvs_close(handle);
}

void ColorChain::save() const
{
    uint8_t blob[kMaxBytes] = {};
    blob[0] = repeat_ ? kFlagRepeat : 0;
    blob[1] = static_cast<uint8_t>(count_);
    for (size_t index = 0; index < count_; ++index) {
        uint8_t *out = blob + 2 + index * kStepBytes;
        out[0] = steps_[index].color.r;
        out[1] = steps_[index].color.g;
        out[2] = steps_[index].color.b;
        put_u16(out + 3, steps_[index].fade_ds);
        put_u16(out + 5, steps_[index].hold_ds);
        out[7] = steps_[index].sound;
    }
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_blob(handle, kStepsKey, blob, 2 + count_ * kStepBytes) != ESP_OK || nvs_commit(handle) != ESP_OK) {
        ESP_LOGW(kTag, "Color chain was not stored");
    }
    nvs_close(handle);
}

bool ColorChain::apply(const uint8_t *data, size_t length, SerialRgbLed::Color current, int64_t now_us)
{
    if (data == nullptr || length == 0) return false;
    if (data[0] == kStop && length == 1) {
        stop();
        return true;
    }
    if (data[0] == kRestart && length == 1) {
        if (count_ == 0) return false;
        start(current, now_us);
        return true;
    }
    if (data[0] != kStore || length < 3) return false;
    const size_t count = data[2];
    if (count == 0 || count > kMaxSteps || length != 3 + count * kStepBytes) return false;
    const bool repeat = (data[1] & kFlagRepeat) != 0;
    uint32_t total_ds = 0;
    Step steps[kMaxSteps] = {};
    for (size_t index = 0; index < count; ++index) {
        const uint8_t *in = data + 3 + index * kStepBytes;
        steps[index] = {{in[0], in[1], in[2]}, get_u16(in + 3), get_u16(in + 5), in[7]};
        total_ds += steps[index].fade_ds + steps[index].hold_ds;
    }
    // A repeating chain that takes no time would never let go of the loop.
    if (repeat && total_ds == 0) return false;
    std::memcpy(steps_, steps, sizeof(steps));
    count_ = count;
    repeat_ = repeat;
    save();
    start(current, now_us);
    ESP_LOGI(kTag, "Color chain started: %u steps%s", static_cast<unsigned>(count_), repeat_ ? ", repeating" : "");
    return true;
}

void ColorChain::start(SerialRgbLed::Color current, int64_t now_us)
{
    running_ = count_ > 0;
    index_ = 0;
    arrived_ = false;
    step_start_us_ = now_us;
    from_ = current;
}

void ColorChain::stop()
{
    if (running_) ESP_LOGI(kTag, "Color chain stopped");
    running_ = false;
}

SerialRgbLed::Color ColorChain::target() const
{
    return count_ == 0 ? SerialRgbLed::Color{} : steps_[std::min(index_, count_ - 1)].color;
}

uint32_t ColorChain::update(int64_t now_us, SerialRgbLed::Color *color, uint8_t *sound)
{
    if (sound != nullptr) *sound = 0;
    if (!running_ || color == nullptr) return UINT32_MAX;
    for (size_t guard = 0; guard <= count_; ++guard) {
        const Step &step = steps_[index_];
        const int64_t fade_us = static_cast<int64_t>(step.fade_ds) * 100000;
        const int64_t hold_us = static_cast<int64_t>(step.hold_ds) * 100000;
        const int64_t elapsed = now_us - step_start_us_;
        if (elapsed < fade_us) {
            *color = {mix(from_.r, step.color.r, elapsed, fade_us), mix(from_.g, step.color.g, elapsed, fade_us),
                      mix(from_.b, step.color.b, elapsed, fade_us)};
            return kFrameMs;
        }
        *color = step.color;
        if (!arrived_) {
            arrived_ = true;
            if (sound != nullptr && step.sound != 0) *sound = step.sound;
        }
        if (elapsed < fade_us + hold_us) {
            return static_cast<uint32_t>(std::max<int64_t>(1, (fade_us + hold_us - elapsed + 999) / 1000));
        }
        from_ = step.color;
        index_ += 1;
        if (index_ >= count_) {
            if (!repeat_) {
                index_ = count_ - 1;
                running_ = false;
                ESP_LOGI(kTag, "Color chain finished");
                return UINT32_MAX;
            }
            index_ = 0;
        }
        // Keep the rhythm unless the loop fell far behind (light sleep, Wi-Fi).
        step_start_us_ += fade_us + hold_us;
        if (now_us - step_start_us_ > 1000000) step_start_us_ = now_us;
        arrived_ = false;
    }
    return kFrameMs;
}

size_t ColorChain::serialize(uint8_t *out, size_t size) const
{
    const size_t length = 4 + count_ * kStepBytes;
    if (out == nullptr || size < length) return 0;
    out[0] = running_ ? 1 : 0;
    out[1] = static_cast<uint8_t>(index_);
    out[2] = repeat_ ? kFlagRepeat : 0;
    out[3] = static_cast<uint8_t>(count_);
    for (size_t index = 0; index < count_; ++index) {
        uint8_t *step = out + 4 + index * kStepBytes;
        step[0] = steps_[index].color.r;
        step[1] = steps_[index].color.g;
        step[2] = steps_[index].color.b;
        put_u16(step + 3, steps_[index].fade_ds);
        put_u16(step + 5, steps_[index].hold_ds);
        step[7] = steps_[index].sound;
    }
    return length;
}
