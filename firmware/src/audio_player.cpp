#include "audio_player.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sdkconfig.h"

#if CONFIG_EMMAFORO_AUDIO

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/sdm.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_pm.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

namespace audio {
namespace {

constexpr char kTag[] = "AUDIO";
constexpr gpio_num_t kShutdownGpio = static_cast<gpio_num_t>(CONFIG_EMMAFORO_AUDIO_SD_GPIO);
constexpr gpio_num_t kSignalGpio = static_cast<gpio_num_t>(CONFIG_EMMAFORO_AUDIO_SIGNAL_GPIO);
constexpr char kNamespace[] = "audio";
constexpr char kVolumeKey[] = "volume";
constexpr char kMapKey[] = "map";
constexpr uint32_t kVoiceMagic = 0x55414D45;  // "EMAU"
constexpr size_t kHeaderBytes = 4096;
constexpr size_t kRingSize = 2048;
constexpr uint32_t kRingMask = kRingSize - 1;
// The coupling capacitor settles at the mid level before the amplifier wakes,
// so it does not click.
constexpr uint32_t kSettleMs = 30;

struct VoiceHeader {
    uint32_t magic;
    uint32_t length;
    uint32_t crc;
    uint32_t rate;
};

struct Note {
    uint16_t hz;  // 0 is a pause
    uint16_t ms;
};

const Note kBip[] = {{1000, 120}};
const Note kDoubleBip[] = {{1000, 100}, {0, 80}, {1000, 100}};
const Note kDinDon[] = {{659, 350}, {523, 550}};
const Note kAlarm[] = {{880, 150}, {660, 150}, {880, 150}, {660, 150}, {880, 150}, {660, 150}};
const Note kTune[] = {{523, 140}, {659, 140}, {784, 140}, {1047, 320}};

struct Tone {
    const Note *notes;
    size_t count;
};

const Tone kTones[kToneCount] = {
    {kBip, sizeof(kBip) / sizeof(kBip[0])},
    {kDoubleBip, sizeof(kDoubleBip) / sizeof(kDoubleBip[0])},
    {kDinDon, sizeof(kDinDon) / sizeof(kDinDon[0])},
    {kAlarm, sizeof(kAlarm) / sizeof(kAlarm[0])},
    {kTune, sizeof(kTune) / sizeof(kTune[0])},
};

DRAM_ATTR int8_t ring[kRingSize] = {};
DRAM_ATTR volatile uint32_t ring_head = 0;
DRAM_ATTR volatile uint32_t ring_tail = 0;
DRAM_ATTR sdm_channel_handle_t sdm = nullptr;
int8_t sine[256] = {};

gptimer_handle_t timer = nullptr;
esp_pm_lock_handle_t pm_lock = nullptr;
QueueHandle_t requests = nullptr;
SemaphoreHandle_t flash_lock = nullptr;
const esp_partition_t *partition = nullptr;
VoiceHeader headers[kVoiceSlots] = {};
volatile bool abort_requested = false;
volatile bool busy = false;
bool ready = false;
uint8_t volume = 70;
uint8_t state_map[static_cast<size_t>(LampState::Count)] = {};
int upload_slot = -1;
uint32_t upload_length = 0;

bool IRAM_ATTR on_sample(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *)
{
    int8_t sample = 0;
    const uint32_t tail = ring_tail;
    if (tail != ring_head) {
        sample = ring[tail & kRingMask];
        ring_tail = tail + 1;
    }
    sdm_channel_set_pulse_density(sdm, sample);
    return false;
}

bool valid_sound(int sound)
{
    return sound == kSoundNone || (sound >= 1 && sound <= kToneCount) ||
           (sound >= kVoiceFirst && sound < kVoiceFirst + kVoiceSlots);
}

uint32_t slot_offset(int slot)
{
    return kHeaderBytes + static_cast<uint32_t>(slot) * kVoiceMaxBytes;
}

void save_settings()
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, kVolumeKey, volume);
    nvs_set_blob(handle, kMapKey, state_map, sizeof(state_map));
    if (nvs_commit(handle) != ESP_OK) ESP_LOGW(kTag, "Audio settings were not stored");
    nvs_close(handle);
}

void load_settings()
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t stored_volume = volume;
    if (nvs_get_u8(handle, kVolumeKey, &stored_volume) == ESP_OK && stored_volume <= 100) volume = stored_volume;
    uint8_t stored_map[sizeof(state_map)] = {};
    size_t length = sizeof(stored_map);
    if (nvs_get_blob(handle, kMapKey, stored_map, &length) == ESP_OK && length == sizeof(stored_map)) {
        for (size_t index = 0; index < sizeof(stored_map); ++index) {
            state_map[index] = valid_sound(stored_map[index]) ? stored_map[index] : kSoundNone;
        }
    }
    nvs_close(handle);
}

esp_err_t save_headers()
{
    if (partition == nullptr) return ESP_ERR_NOT_FOUND;
    esp_err_t ret = esp_partition_erase_range(partition, 0, kHeaderBytes);
    if (ret == ESP_OK) ret = esp_partition_write(partition, 0, headers, sizeof(headers));
    return ret;
}

bool voice_present(int slot)
{
    return headers[slot].magic == kVoiceMagic && headers[slot].length > 0 &&
           headers[slot].length <= kVoiceMaxBytes;
}

bool should_stop()
{
    return abort_requested || uxQueueMessagesWaiting(requests) > 0;
}

bool push(int8_t sample)
{
    while (ring_head - ring_tail >= kRingSize) {
        if (should_stop()) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ring[ring_head & kRingMask] = static_cast<int8_t>((static_cast<int>(sample) * volume) / 100);
    ring_head = ring_head + 1;
    return true;
}

bool play_tone(const Tone &tone)
{
    for (size_t index = 0; index < tone.count; ++index) {
        const Note &note = tone.notes[index];
        const uint32_t total = note.ms * kSampleRate / 1000;
        const uint32_t attack = kSampleRate / 200;
        const uint32_t release = total * 3 / 10;
        const uint32_t step = static_cast<uint32_t>((static_cast<uint64_t>(note.hz) << 32) / kSampleRate);
        uint32_t phase = 0;
        for (uint32_t sample = 0; sample < total; ++sample) {
            int32_t level = 0;
            if (note.hz != 0) {
                int32_t envelope = 256;
                if (sample < attack) envelope = static_cast<int32_t>(sample * 256 / attack);
                if (sample + release > total) envelope = static_cast<int32_t>((total - sample) * 256 / release);
                level = sine[phase >> 24] * envelope / 256;
                phase += step;
            }
            if (!push(static_cast<int8_t>(level))) return false;
        }
    }
    return true;
}

bool play_voice(int slot)
{
    if (partition == nullptr || !voice_present(slot)) return false;
    const uint32_t length = headers[slot].length;
    int8_t chunk[256];
    for (uint32_t offset = 0; offset < length; offset += sizeof(chunk)) {
        const uint32_t count = std::min<uint32_t>(sizeof(chunk), length - offset);
        xSemaphoreTake(flash_lock, portMAX_DELAY);
        const esp_err_t ret = esp_partition_read(partition, slot_offset(slot) + offset, chunk, count);
        xSemaphoreGive(flash_lock);
        if (ret != ESP_OK) return false;
        for (uint32_t index = 0; index < count; ++index) {
            if (!push(chunk[index])) return false;
        }
    }
    return true;
}

void hardware_on()
{
    if (pm_lock != nullptr) esp_pm_lock_acquire(pm_lock);
    ring_tail = ring_head;
    sdm_channel_enable(sdm);
    sdm_channel_set_pulse_density(sdm, 0);
    gptimer_enable(timer);
    gptimer_start(timer);
    vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    gpio_set_level(kShutdownGpio, 1);
    vTaskDelay(pdMS_TO_TICKS(kSettleMs));
}

void hardware_off()
{
    gpio_set_level(kShutdownGpio, 0);
    gptimer_stop(timer);
    gptimer_disable(timer);
    sdm_channel_set_pulse_density(sdm, 0);
    sdm_channel_disable(sdm);
    ring_tail = ring_head;
    if (pm_lock != nullptr) esp_pm_lock_release(pm_lock);
}

void audio_task(void *)
{
    uint8_t sound = 0;
    while (true) {
        if (xQueueReceive(requests, &sound, portMAX_DELAY) != pdTRUE) continue;
        while (xQueueReceive(requests, &sound, 0) == pdTRUE) {
        }
        if (sound == kSoundNone) continue;
        abort_requested = false;
        busy = true;
        hardware_on();
        bool finished = false;
        if (sound >= 1 && sound <= kToneCount) {
            finished = play_tone(kTones[sound - 1]);
        } else if (sound >= kVoiceFirst && sound < kVoiceFirst + kVoiceSlots) {
            finished = play_voice(sound - kVoiceFirst);
        }
        while (finished && ring_tail != ring_head && !should_stop()) vTaskDelay(pdMS_TO_TICKS(5));
        hardware_off();
        busy = false;
    }
}

bool parse_state(const char *text, LampState *state)
{
    static const char *const kNames[] = {"rosso", "giallo", "verde", "spento", "altro"};
    for (size_t index = 0; index < sizeof(kNames) / sizeof(kNames[0]); ++index) {
        if (std::strcmp(text, kNames[index]) == 0) {
            *state = static_cast<LampState>(index);
            return true;
        }
    }
    return false;
}

void wait_idle()
{
    stop();
    for (int attempt = 0; attempt < 100 && busy; ++attempt) vTaskDelay(pdMS_TO_TICKS(5));
}

bool clip_command(const char *command)
{
    if (partition == nullptr) return false;
    char *cursor = nullptr;
    if (std::strncmp(command, "begin:", 6) == 0) {
        const long slot = std::strtol(command + 6, &cursor, 10);
        if (cursor == nullptr || *cursor != ':' || slot < 0 || slot >= kVoiceSlots) return false;
        const unsigned long length = std::strtoul(cursor + 1, nullptr, 10);
        if (length == 0 || length > kVoiceMaxBytes) return false;
        wait_idle();
        xSemaphoreTake(flash_lock, portMAX_DELAY);
        headers[slot] = {};
        esp_err_t ret = save_headers();
        const uint32_t erase = (static_cast<uint32_t>(length) + 4095U) & ~4095U;
        if (ret == ESP_OK) ret = esp_partition_erase_range(partition, slot_offset(slot), erase);
        xSemaphoreGive(flash_lock);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "Voice slot %ld not erased: %s", slot, esp_err_to_name(ret));
            upload_slot = -1;
            return false;
        }
        upload_slot = static_cast<int>(slot);
        upload_length = static_cast<uint32_t>(length);
        ESP_LOGI(kTag, "Voice slot %ld: receiving %lu bytes", slot, length);
        return true;
    }
    if (std::strncmp(command, "end:", 4) == 0) {
        const long slot = std::strtol(command + 4, &cursor, 10);
        if (cursor == nullptr || *cursor != ':' || slot != upload_slot) return false;
        const uint32_t expected = static_cast<uint32_t>(std::strtoul(cursor + 1, nullptr, 16));
        uint32_t crc = 0;
        uint8_t chunk[256];
        xSemaphoreTake(flash_lock, portMAX_DELAY);
        esp_err_t ret = ESP_OK;
        for (uint32_t offset = 0; ret == ESP_OK && offset < upload_length; offset += sizeof(chunk)) {
            const uint32_t count = std::min<uint32_t>(sizeof(chunk), upload_length - offset);
            ret = esp_partition_read(partition, slot_offset(slot) + offset, chunk, count);
            crc = esp_rom_crc32_le(crc, chunk, count);
        }
        if (ret == ESP_OK && crc == expected) {
            headers[slot] = {kVoiceMagic, upload_length, crc, kSampleRate};
            ret = save_headers();
        }
        xSemaphoreGive(flash_lock);
        const bool stored = ret == ESP_OK && crc == expected;
        ESP_LOGI(kTag, "Voice slot %ld %s (crc %08lx, expected %08lx)", slot, stored ? "stored" : "rejected",
                 static_cast<unsigned long>(crc), static_cast<unsigned long>(expected));
        upload_slot = -1;
        return stored;
    }
    if (std::strncmp(command, "del:", 4) == 0) {
        const long slot = std::strtol(command + 4, nullptr, 10);
        if (slot < 0 || slot >= kVoiceSlots) return false;
        wait_idle();
        xSemaphoreTake(flash_lock, portMAX_DELAY);
        headers[slot] = {};
        const esp_err_t ret = save_headers();
        xSemaphoreGive(flash_lock);
        return ret == ESP_OK;
    }
    return false;
}

}  // namespace

esp_err_t init()
{
    if (ready) return ESP_OK;
    for (int index = 0; index < 256; ++index) {
        sine[index] = static_cast<int8_t>(std::lround(std::sin(index * 2.0 * M_PI / 256.0) * 120.0));
    }
    gpio_config_t shutdown = {};
    shutdown.pin_bit_mask = 1ULL << kShutdownGpio;
    shutdown.mode = GPIO_MODE_OUTPUT;
    esp_err_t ret = gpio_config(&shutdown);
    if (ret != ESP_OK) return ret;
    gpio_set_level(kShutdownGpio, 0);

    sdm_config_t sdm_config = {};
    sdm_config.gpio_num = kSignalGpio;
    sdm_config.clk_src = SDM_CLK_SRC_DEFAULT;
    sdm_config.sample_rate_hz = 1000000;
    ret = sdm_new_channel(&sdm_config, &sdm);
    if (ret != ESP_OK) return ret;

    gptimer_config_t timer_config = {};
    timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timer_config.direction = GPTIMER_COUNT_UP;
    timer_config.resolution_hz = 1000000;
    ret = gptimer_new_timer(&timer_config, &timer);
    if (ret != ESP_OK) return ret;
    gptimer_alarm_config_t alarm = {};
    alarm.alarm_count = 1000000 / kSampleRate;
    alarm.reload_count = 0;
    alarm.flags.auto_reload_on_alarm = true;
    ret = gptimer_set_alarm_action(timer, &alarm);
    if (ret != ESP_OK) return ret;
    gptimer_event_callbacks_t callbacks = {};
    callbacks.on_alarm = on_sample;
    ret = gptimer_register_event_callbacks(timer, &callbacks, nullptr);
    if (ret != ESP_OK) return ret;

#if CONFIG_PM_ENABLE
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "audio", &pm_lock) != ESP_OK) pm_lock = nullptr;
#endif

    flash_lock = xSemaphoreCreateMutex();
    requests = xQueueCreate(4, sizeof(uint8_t));
    if (flash_lock == nullptr || requests == nullptr) return ESP_ERR_NO_MEM;

    // Boards updated over the air keep their old partition table, which has
    // no room for voices: they still play the built-in tones.
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "audio");
    if (partition != nullptr && esp_partition_read(partition, 0, headers, sizeof(headers)) != ESP_OK) {
        std::memset(headers, 0, sizeof(headers));
    }
    if (partition == nullptr) ESP_LOGI(kTag, "No audio partition: built-in tones only");

    load_settings();
    if (xTaskCreate(audio_task, "audio", 3072, nullptr, 6, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    ready = true;
    unsigned voices = 0;
    for (int slot = 0; slot < kVoiceSlots; ++slot) voices += voice_present(slot) ? 1 : 0;
    ESP_LOGI(kTag, "Speaker ready: volume %u, %u voice messages", volume, voices);
    return ESP_OK;
}

bool enabled()
{
    return ready;
}

void play(uint8_t sound)
{
    if (!ready || !valid_sound(sound) || sound == kSoundNone) return;
    abort_requested = true;
    if (xQueueSend(requests, &sound, 0) != pdTRUE) {
        xQueueReset(requests);
        xQueueSend(requests, &sound, 0);
    }
}

void play_for_state(LampState state)
{
    if (state >= LampState::Count) return;
    play(state_map[static_cast<size_t>(state)]);
}

void stop()
{
    if (!ready) return;
    xQueueReset(requests);
    abort_requested = true;
}

bool playing()
{
    return busy;
}

bool handle_command(const char *command)
{
    if (!ready || command == nullptr) return false;
    if (std::strncmp(command, "vol:", 4) == 0) {
        const long value = std::strtol(command + 4, nullptr, 10);
        volume = static_cast<uint8_t>(value < 0 ? 0 : value > 100 ? 100 : value);
        save_settings();
        return true;
    }
    if (std::strncmp(command, "map:", 4) == 0) {
        char name[12] = {};
        const char *colon = std::strchr(command + 4, ':');
        if (colon == nullptr || static_cast<size_t>(colon - command - 4) >= sizeof(name)) return false;
        std::memcpy(name, command + 4, static_cast<size_t>(colon - command - 4));
        LampState state = LampState::Altro;
        const long sound = std::strtol(colon + 1, nullptr, 10);
        if (!parse_state(name, &state) || !valid_sound(static_cast<int>(sound))) return false;
        state_map[static_cast<size_t>(state)] = static_cast<uint8_t>(sound);
        save_settings();
        return true;
    }
    if (std::strncmp(command, "play:", 5) == 0) {
        const long sound = std::strtol(command + 5, nullptr, 10);
        if (!valid_sound(static_cast<int>(sound))) return false;
        play(static_cast<uint8_t>(sound));
        return true;
    }
    if (std::strcmp(command, "stop") == 0) {
        stop();
        return true;
    }
    if (std::strncmp(command, "clip:", 5) == 0) return clip_command(command + 5);
    return false;
}

bool handle_clip_data(const uint8_t *data, size_t length)
{
    if (!ready || partition == nullptr || upload_slot < 0 || data == nullptr || length <= 4) return false;
    const uint32_t offset = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                            (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    const size_t count = length - 4;
    if (offset + count > upload_length) return false;
    xSemaphoreTake(flash_lock, portMAX_DELAY);
    const esp_err_t ret = esp_partition_write(partition, slot_offset(upload_slot) + offset, data + 4, count);
    xSemaphoreGive(flash_lock);
    return ret == ESP_OK;
}

int format_status(char *text, size_t size)
{
    unsigned mask = 0;
    for (int slot = 0; slot < kVoiceSlots; ++slot) {
        if (voice_present(slot)) mask |= 1u << slot;
    }
    return std::snprintf(text, size, "1\t%u\t%u,%u,%u,%u,%u\t%u\t%lu\t%u", volume, state_map[0], state_map[1],
                         state_map[2], state_map[3], state_map[4], mask,
                         static_cast<unsigned long>(kVoiceMaxBytes * 1000 / kSampleRate), partition != nullptr ? 1 : 0);
}

}  // namespace audio

#else

namespace audio {
esp_err_t init() { return ESP_ERR_NOT_SUPPORTED; }
bool enabled() { return false; }
void play(uint8_t) {}
void play_for_state(LampState) {}
void stop() {}
bool playing() { return false; }
bool handle_command(const char *) { return false; }
bool handle_clip_data(const uint8_t *, size_t) { return false; }
int format_status(char *text, size_t size) { return text != nullptr && size > 0 ? (text[0] = '\0', 0) : 0; }
}  // namespace audio

#endif
