#include "power_debug.h"

#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#if CONFIG_EMMAFORO_DEBUG_POWER

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace power_debug {
namespace {

constexpr char kTag[] = "PWR";
constexpr uint32_t kPeriodMs = CONFIG_EMMAFORO_DEBUG_POWER_PERIOD_S * 1000U;
constexpr uint32_t kAudioPollMs = 250;
// The cell needs a moment to recover before the "after" voltage means anything.
constexpr uint32_t kAudioRecoverMs = 1000;
constexpr uint32_t kHeaderEveryRows = 120;
constexpr uint32_t kSectorBytes = 4096;
constexpr uint32_t kSeqMask = 0x00FFFFFF;
constexpr uint32_t kTag8 = 0xA7000000;
constexpr uint16_t kUnknown = 0xFFFF;
constexpr uint32_t kCellMilliampHours = 900;
// Average since unplugging once this much is measured; before that the gauge rate.
constexpr int64_t kSlopeMinUs = 600LL * 1000000LL;
constexpr int32_t kSlopeMinDropTenths = 5;

constexpr uint8_t kRegVbusStatus = 0x0B;
constexpr uint8_t kPowerGoodMask = 0x04;
constexpr uint8_t kRegVbusAdc = 0x11;
constexpr uint8_t kRegFault = 0x0C;

constexpr uint8_t kFlagCharging = 0x01;
constexpr uint8_t kFlagPowerGood = 0x02;
constexpr uint8_t kFlagLampOn = 0x04;
constexpr uint8_t kFlagBlink = 0x08;
constexpr uint8_t kFlagBle = 0x10;
constexpr uint8_t kFlagWifi = 0x20;
constexpr uint8_t kFlagResting = 0x40;
constexpr uint8_t kFlagAudio = 0x80;

constexpr uint8_t kChargeStateMask = 0x07;
constexpr uint8_t kChargeStateUnknown = 7;
constexpr uint8_t kStatusGauge = 0x08;
constexpr uint8_t kStatusFault = 0x10;

// tools/power_log.py reads this layout. Little-endian, 32 bytes.
struct __attribute__((packed)) Record {
    uint32_t seq;  // top byte is a fixed tag
    uint32_t uptime_s;
    uint16_t voltage_mv;
    uint16_t soc_x10;
    int16_t rate_centi;  // 0.01 %/h
    uint16_t sleep_permille;  // time in light sleep since the last row
    uint16_t wakeups;         // light sleep exits since the last row
    uint16_t autonomy_min;
    uint16_t audio_ms;  // audio row: length of the sound, else 0
    uint8_t sag_mv;     // audio row: voltage minus the lowest seen during the sound
    uint8_t charge_50ma;
    uint8_t vbus_100mv;
    uint8_t flags;
    uint8_t status;  // charge state, gauge, fault
    uint8_t r, g, b;
    uint8_t sound;  // 0 on a periodic row
    uint8_t check;
};
static_assert(sizeof(Record) == 32, "Record is part of the log format");
constexpr uint32_t kRecordsPerSector = kSectorBytes / sizeof(Record);

BQ *charger_chip = nullptr;
Max17048 *gauge_chip = nullptr;
TaskHandle_t task_handle = nullptr;
const esp_partition_t *partition = nullptr;
esp_pm_lock_handle_t usb_lock = nullptr;
bool usb_lock_held = false;

portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
State shared_state = {};

volatile uint8_t audio_sound = 0;
volatile bool audio_active = false;
volatile int64_t audio_start_us = 0;
volatile int64_t audio_stop_us = 0;

// Written in the idle task. 32 bits wrap every 71 minutes, the differences do not care.
volatile uint32_t slept_us_total = 0;
volatile uint32_t wakeups_total = 0;

uint32_t write_offset = 0;
uint32_t next_seq = 0;
uint32_t rows_printed = 0;

uint32_t last_slept_us = 0;
uint32_t last_wakeups = 0;
int64_t last_row_us = 0;

int64_t unplug_us = 0;
int32_t unplug_soc_x10 = -1;

esp_err_t IRAM_ATTR on_sleep_exit(int64_t slept_us, void *)
{
    slept_us_total = slept_us_total + static_cast<uint32_t>(slept_us);
    wakeups_total = wakeups_total + 1;
    return ESP_OK;
}

uint8_t checksum(const Record &record)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
    uint8_t sum = 0;
    for (size_t index = 0; index + 1 < sizeof(Record); ++index) {
        sum = static_cast<uint8_t>(sum * 31U + bytes[index]);
    }
    return sum;
}

bool read_valid(uint32_t offset, Record *record)
{
    return esp_partition_read(partition, offset, record, sizeof(*record)) == ESP_OK &&
           (record->seq & ~kSeqMask) == kTag8 && record->check == checksum(*record);
}

bool slot_is_free(const Record &record)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
    for (size_t index = 0; index < sizeof(Record); ++index) {
        if (bytes[index] != 0xFF) return false;
    }
    return true;
}

// Finds where the log stopped: the sector whose first record is the newest,
// then the first free slot in it. Stale bytes from an old ota_1 image fail the check.
void find_write_position()
{
    uint32_t best_sector = 0;
    uint32_t best_seq = 0;
    bool found = false;
    Record record = {};
    for (uint32_t offset = 0; offset < partition->size; offset += kSectorBytes) {
        if (!read_valid(offset, &record)) continue;
        const uint32_t seq = record.seq & kSeqMask;
        if (!found || seq > best_seq) {
            found = true;
            best_seq = seq;
            best_sector = offset;
        }
    }
    if (!found) return;
    uint32_t last_seq = best_seq;
    uint32_t slot = 1;
    for (; slot < kRecordsPerSector; ++slot) {
        const uint32_t offset = best_sector + slot * sizeof(Record);
        if (read_valid(offset, &record)) {
            last_seq = record.seq & kSeqMask;
        } else if (slot_is_free(record)) {
            break;
        }
        // Otherwise a write cut short by a reset: it cannot be written again, skip it.
    }
    write_offset = best_sector + slot * sizeof(Record);
    if (write_offset >= partition->size) write_offset = 0;
    next_seq = (last_seq + 1) & kSeqMask;
}

void store(Record &record)
{
    if (partition == nullptr) return;
    if (write_offset % kSectorBytes == 0) {
        const esp_err_t erased = esp_partition_erase_range(partition, write_offset, kSectorBytes);
        if (erased != ESP_OK) {
            ESP_LOGW(kTag, "Log sector not erased: %s", esp_err_to_name(erased));
            return;
        }
    }
    const esp_err_t written = esp_partition_write(partition, write_offset, &record, sizeof(record));
    if (written != ESP_OK) ESP_LOGW(kTag, "Log record not written: %s", esp_err_to_name(written));
    write_offset += sizeof(record);
    if (write_offset >= partition->size) write_offset = 0;
}

void print_row(const Record &record)
{
    if (rows_printed % kHeaderEveryRows == 0) {
        std::printf("PWR,seq,uptime_s,type,mv,min_mv,soc_x10,rate_centi,drain_ma,charge_ma,vbus_mv,charge_state,"
                    "power_good,charging,gauge,fault,r,g,b,lamp_on,blink,ble,wifi,resting,audio,sound,audio_ms,"
                    "sleep_permille,wakeups,autonomy_min\n");
    }
    rows_printed++;
    const int32_t drain_ma = record.rate_centi < 0
        ? -static_cast<int32_t>(record.rate_centi) * static_cast<int32_t>(kCellMilliampHours) / 10000
        : 0;
    std::printf("PWR,%lu,%lu,%s,%u,%u,%u,%d,%ld,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                static_cast<unsigned long>(record.seq & kSeqMask), static_cast<unsigned long>(record.uptime_s),
                record.sound != 0 ? "audio" : "periodic", record.voltage_mv, record.voltage_mv - record.sag_mv,
                record.soc_x10, record.rate_centi, static_cast<long>(drain_ma), record.charge_50ma * 50U,
                record.vbus_100mv * 100U,
                record.status & kChargeStateMask, (record.flags & kFlagPowerGood) != 0,
                (record.flags & kFlagCharging) != 0, (record.status & kStatusGauge) != 0,
                (record.status & kStatusFault) != 0, record.r, record.g, record.b,
                (record.flags & kFlagLampOn) != 0, (record.flags & kFlagBlink) != 0, (record.flags & kFlagBle) != 0,
                (record.flags & kFlagWifi) != 0, (record.flags & kFlagResting) != 0,
                (record.flags & kFlagAudio) != 0, record.sound, record.audio_ms, record.sleep_permille,
                record.wakeups, record.autonomy_min);
    std::fflush(stdout);
}

bool read_voltage(uint16_t *millivolts)
{
    if (gauge_chip != nullptr && gauge_chip->read_voltage_mv(millivolts) == ESP_OK && *millivolts > 0) return true;
    return charger_chip != nullptr && charger_chip->BQ_get_battery_voltage_mv(millivolts) == ESP_OK;
}

uint16_t autonomy_minutes(const Record &record, int64_t now_us)
{
    if ((record.flags & kFlagPowerGood) != 0 || record.soc_x10 == kUnknown || record.soc_x10 == 0) return kUnknown;
    const int64_t elapsed_us = now_us - unplug_us;
    const int32_t dropped = unplug_soc_x10 - static_cast<int32_t>(record.soc_x10);
    int64_t minutes = -1;
    if (unplug_soc_x10 >= 0 && elapsed_us >= kSlopeMinUs && dropped >= kSlopeMinDropTenths) {
        minutes = static_cast<int64_t>(record.soc_x10) * elapsed_us / dropped / 60000000LL;
    } else if (record.rate_centi < 0) {
        // soc_x10 is 0.1 %, rate is 0.01 %/h.
        minutes = static_cast<int64_t>(record.soc_x10) * 10 * 60 / -record.rate_centi;
    }
    if (minutes < 0) return kUnknown;
    return minutes >= kUnknown ? static_cast<uint16_t>(kUnknown - 1) : static_cast<uint16_t>(minutes);
}

void fill_row(Record *record, int64_t now_us)
{
    State state = {};
    taskENTER_CRITICAL(&state_lock);
    state = shared_state;
    taskEXIT_CRITICAL(&state_lock);

    *record = {};
    record->uptime_s = static_cast<uint32_t>(now_us / 1000000LL);
    record->soc_x10 = kUnknown;
    uint16_t voltage_mv = 0;
    if (read_voltage(&voltage_mv)) record->voltage_mv = voltage_mv;

    uint16_t soc = 0;
    if (gauge_chip != nullptr && gauge_chip->read_soc_x256(&soc) == ESP_OK) {
        const uint32_t tenths = (static_cast<uint32_t>(soc) * 10U + 128U) / 256U;
        record->soc_x10 = static_cast<uint16_t>(tenths > 1000U ? 1000U : tenths);
        record->status |= kStatusGauge;
        int16_t rate = 0;
        if (gauge_chip->read_rate_centi_percent_per_hour(&rate) == ESP_OK) record->rate_centi = rate;
    } else if (state.percent != 0) {
        record->soc_x10 = static_cast<uint16_t>(state.percent * 10U);
    }

    uint8_t charge_state = kChargeStateUnknown;
    if (charger_chip != nullptr) {
        BQ::ChargeStatus charge = BQ::ChargeStatus::Unknown;
        if (charger_chip->BQ_get_charge_status(&charge) == ESP_OK && charge != BQ::ChargeStatus::Unknown) {
            charge_state = static_cast<uint8_t>(charge);
            if (charge == BQ::ChargeStatus::PreCharge || charge == BQ::ChargeStatus::FastCharge) {
                record->flags |= kFlagCharging;
                uint16_t charge_ma = 0;
                if (charger_chip->BQ_get_charge_current_ma(&charge_ma) == ESP_OK) {
                    record->charge_50ma = static_cast<uint8_t>(charge_ma / 50U);
                }
            }
        }
        uint8_t value = 0;
        if (charger_chip->BQ_read_register(kRegVbusStatus, &value) == ESP_OK && (value & kPowerGoodMask) != 0) {
            record->flags |= kFlagPowerGood;
        }
        // VBUS_GD in bit 7, then 100 mV steps above 2.6 V.
        if (charger_chip->BQ_read_register(kRegVbusAdc, &value) == ESP_OK && (value & 0x80) != 0) {
            record->vbus_100mv = static_cast<uint8_t>(26U + (value & 0x7FU));
        }
        if (charger_chip->BQ_read_register(kRegFault, &value) == ESP_OK && value != 0) record->status |= kStatusFault;
    }
    record->status |= charge_state & kChargeStateMask;

    if (state.lamp_on) record->flags |= kFlagLampOn;
    if (state.blink) record->flags |= kFlagBlink;
    if (state.ble_connected) record->flags |= kFlagBle;
    if (state.wifi_on) record->flags |= kFlagWifi;
    if (state.resting) record->flags |= kFlagResting;
    if (state.lamp_on) {
        record->r = state.r;
        record->g = state.g;
        record->b = state.b;
    }

    const uint32_t slept = slept_us_total;
    const uint32_t wakeups = wakeups_total;
    const int64_t elapsed_us = now_us - last_row_us;
    if (last_row_us != 0 && elapsed_us > 0) {
        const uint64_t permille = static_cast<uint64_t>(slept - last_slept_us) * 1000ULL / elapsed_us;
        record->sleep_permille = static_cast<uint16_t>(permille > 1000U ? 1000U : permille);
    }
    record->wakeups = static_cast<uint16_t>(wakeups - last_wakeups);
    last_slept_us = slept;
    last_wakeups = wakeups;
    last_row_us = now_us;

    // Light sleep stops the USB serial port: stay awake only while USB is there.
    const bool usb_present = (record->flags & kFlagPowerGood) != 0;
    if (usb_lock != nullptr && usb_present != usb_lock_held) {
        if (usb_present) {
            esp_pm_lock_acquire(usb_lock);
        } else {
            esp_pm_lock_release(usb_lock);
        }
        usb_lock_held = usb_present;
    }

    if (usb_present) {
        unplug_soc_x10 = -1;
    } else if (unplug_soc_x10 < 0 && record->soc_x10 != kUnknown) {
        unplug_us = now_us;
        unplug_soc_x10 = record->soc_x10;
    }
    record->autonomy_min = autonomy_minutes(*record, now_us);
}

void emit(Record &record)
{
    record.seq = kTag8 | next_seq;
    next_seq = (next_seq + 1) & kSeqMask;
    record.check = checksum(record);
    store(record);
    // The serial port exists only through USB: on battery nothing would read
    // it, and waiting for its buffer would change what is being measured.
    if ((record.flags & kFlagPowerGood) != 0) print_row(record);
}

void power_task(void *)
{
    int64_t next_row_us = 0;
    int64_t handled_start_us = 0;
    uint8_t sound = 0;
    int64_t sound_start_us = 0;
    uint16_t sound_min_mv = 0;
    bool measuring = false;

    while (true) {
        const int64_t now_us = esp_timer_get_time();

        // Checking the start time too: a short bip can end before this task runs.
        if (!measuring && audio_start_us != handled_start_us) {
            measuring = true;
            sound = audio_sound;
            sound_start_us = audio_start_us;
            sound_min_mv = 0;
        }

        if (measuring) {
            // At the pace of the gauge and without touching flash, which would
            // hold off the sample interrupt.
            uint16_t voltage_mv = 0;
            if (read_voltage(&voltage_mv) && voltage_mv != 0 && (sound_min_mv == 0 || voltage_mv < sound_min_mv)) {
                sound_min_mv = voltage_mv;
            }
            if (audio_active) sound = audio_sound;
            if (!audio_active && now_us - audio_stop_us >= kAudioRecoverMs * 1000LL) {
                Record record = {};
                fill_row(&record, now_us);
                record.sound = sound;
                record.flags |= kFlagAudio;
                const int64_t length_ms = (audio_stop_us - sound_start_us) / 1000;
                record.audio_ms = length_ms > 65535 ? 65535 : static_cast<uint16_t>(length_ms);
                if (sound_min_mv != 0 && record.voltage_mv > sound_min_mv) {
                    const uint32_t sag = record.voltage_mv - sound_min_mv;
                    record.sag_mv = static_cast<uint8_t>(sag > 255U ? 255U : sag);
                }
                emit(record);
                handled_start_us = audio_start_us;
                measuring = false;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kAudioPollMs));
            continue;
        }

        if (now_us >= next_row_us) {
            Record record = {};
            fill_row(&record, now_us);
            emit(record);
            next_row_us = now_us + static_cast<int64_t>(kPeriodMs) * 1000;
        }
        const int64_t wait_us = next_row_us - esp_timer_get_time();
        const uint32_t wait_ms = wait_us <= 0 ? 1 : static_cast<uint32_t>(wait_us / 1000) + 1;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

}  // namespace

void begin(BQ *charger, Max17048 *gauge, uint8_t percent)
{
    charger_chip = charger;
    gauge_chip = gauge;
    shared_state.percent = percent;

    // A sound lasts a second or two: the voltage has to be fresh during it.
    if (gauge_chip != nullptr && gauge_chip->set_hibernate_allowed(false) != ESP_OK) {
        ESP_LOGW(kTag, "Gauge may hibernate: voltages can be 45 s old");
    }

    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "powerlog");
    if (partition != nullptr) {
        find_write_position();
    } else {
        ESP_LOGW(kTag, "No powerlog partition: serial output only");
    }

    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "pwr_usb", &usb_lock) != ESP_OK) usb_lock = nullptr;

    esp_pm_sleep_cbs_register_config_t callbacks = {};
    callbacks.exit_cb = on_sleep_exit;
    if (esp_pm_light_sleep_register_cbs(&callbacks) != ESP_OK) {
        ESP_LOGW(kTag, "Light sleep time will read 0");
    }

    if (xTaskCreate(power_task, "power_debug", 4096, nullptr, 2, &task_handle) != pdPASS) {
        ESP_LOGE(kTag, "Power log did not start");
        task_handle = nullptr;
        return;
    }
    ESP_LOGI(kTag, "Power log: a row every %u s, next %lu at 0x%lx", CONFIG_EMMAFORO_DEBUG_POWER_PERIOD_S,
             static_cast<unsigned long>(next_seq), static_cast<unsigned long>(write_offset));
}

void set_state(const State &state)
{
    taskENTER_CRITICAL(&state_lock);
    shared_state = state;
    taskEXIT_CRITICAL(&state_lock);
}

void audio_started(uint8_t sound)
{
    audio_sound = sound;
    audio_start_us = esp_timer_get_time();
    audio_active = true;
    if (task_handle != nullptr) xTaskNotifyGive(task_handle);
}

void audio_stopped()
{
    audio_stop_us = esp_timer_get_time();
    audio_active = false;
    if (task_handle != nullptr) xTaskNotifyGive(task_handle);
}

}  // namespace power_debug

#else

namespace power_debug {
void begin(BQ *, Max17048 *, uint8_t) {}
void set_state(const State &) {}
void audio_started(uint8_t) {}
void audio_stopped() {}
}  // namespace power_debug

#endif
