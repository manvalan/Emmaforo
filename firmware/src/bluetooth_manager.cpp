#include "bluetooth_manager.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace {
constexpr char kTag[] = "BT_CONFIG";
constexpr size_t kCredentialSize = 64;
constexpr size_t kDeviceNameSize = 32;
constexpr size_t kColorPresetSize = 25;
constexpr size_t kSerialSize = 18;
BluetoothManager *active_manager = nullptr;
char pending_ssid[kCredentialSize] = {};
char pending_password[kCredentialSize] = {};
bool apply_requested = false;
bool connected = false;
int64_t connected_since_us = 0;
constexpr int64_t kHeldSessionUs = 3LL * 1000000LL;
bool scan_requested = false;
char scan_results[1024] = {};
bool led_command_requested = false;
char pending_led_command[32] = {};
bool led_mode_command_requested = false;
bool pending_configuration_mode = false;
bool device_name_requested = false;
bool safe_charging_mode = true;
bool safe_charging_mode_requested = false;
char device_name[kDeviceNameSize] = "Emmaforo";
char pending_device_name[kDeviceNameSize] = {};
uint8_t battery_percentage = 0;
uint16_t battery_voltage_mv = 0;
bool battery_charging = false;
bool battery_measurement_valid = false;
uint8_t battery_charge_status = 255;
uint8_t battery_fault = 0;
bool battery_safe = true;
bool battery_paused = false;
bool battery_gentle = false;
bool battery_estimate_valid = false;
bool charge_pause_requested = false;
bool pending_pause_value = false;
bool gentle_charge_requested = false;
bool pending_gentle_value = false;
bool shutdown_requested = false;
char network_status[96] = {};
char last_battery_notification[48] = {};
uint8_t color_preset[kColorPresetSize] = {};
uint8_t pending_color_preset[kColorPresetSize] = {};
bool color_preset_requested = false;
char serial_number[kSerialSize] = {};
char pending_firmware_version[32] = {};
bool firmware_approval_requested = false;
wifi_ap_record_t scan_records[20] = {};
uint16_t scan_results_handle = 0;
uint16_t network_status_handle = 0;
uint16_t battery_handle = 0;
uint16_t scan_conn_handle = BLE_HS_CONN_HANDLE_NONE;
TaskHandle_t app_task_handle = nullptr;

void wake_app_task()
{
    if (app_task_handle != nullptr) xTaskNotifyGive(app_task_handle);
}

const ble_uuid16_t kServiceUuid = BLE_UUID16_INIT(0xFFF0);
const ble_uuid16_t kSsidUuid = BLE_UUID16_INIT(0xFFF1);
const ble_uuid16_t kPasswordUuid = BLE_UUID16_INIT(0xFFF2);
const ble_uuid16_t kApplyUuid = BLE_UUID16_INIT(0xFFF3);
const ble_uuid16_t kScanRequestUuid = BLE_UUID16_INIT(0xFFF4);
const ble_uuid16_t kScanResultsUuid = BLE_UUID16_INIT(0xFFF5);
const ble_uuid16_t kLedCommandUuid = BLE_UUID16_INIT(0xFFF6);
const ble_uuid16_t kNetworkStatusUuid = BLE_UUID16_INIT(0xFFF7);
const ble_uuid16_t kLedModeUuid = BLE_UUID16_INIT(0xFFF8);
const ble_uuid16_t kBatteryUuid = BLE_UUID16_INIT(0xFFF9);
const ble_uuid16_t kDeviceNameUuid = BLE_UUID16_INIT(0xFFFB);
const ble_uuid16_t kSafeChargingUuid = BLE_UUID16_INIT(0xFFFC);
const ble_uuid16_t kPowerUuid = BLE_UUID16_INIT(0xFFFA);
const ble_uuid16_t kColorPresetUuid = BLE_UUID16_INIT(0xFFFD);
const ble_uuid16_t kSerialUuid = BLE_UUID16_INIT(0xFFFE);
const ble_uuid16_t kFirmwareVersionUuid = BLE_UUID16_INIT(0xFFEF);
const ble_uuid16_t kFirmwareApproveUuid = BLE_UUID16_INIT(0xFFEE);

int format_battery(char *measurement, size_t size)
{
    return std::snprintf(measurement, size, "%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u",
                         battery_percentage, battery_voltage_mv,
                         battery_charging ? 1 : 0, battery_charge_status,
                         battery_fault, battery_safe ? 1 : 0,
                         battery_paused ? 1 : 0, battery_gentle ? 1 : 0,
                         battery_estimate_valid ? 1 : 0);
}
int on_gap_event(struct ble_gap_event *event, void *arg);

void notify_scan_results()
{
    if (scan_conn_handle == BLE_HS_CONN_HANDLE_NONE || scan_results_handle == 0) return;
    constexpr size_t chunk_size = 20;
    const size_t length = std::strlen(scan_results);
    for (size_t offset = 0; offset < length; offset += chunk_size) {
        const uint16_t chunk = static_cast<uint16_t>(std::min<size_t>(chunk_size, length - offset));
        struct os_mbuf *mbuf = ble_hs_mbuf_from_flat(scan_results + offset, chunk);
        if (mbuf == nullptr || ble_gatts_notify_custom(scan_conn_handle, scan_results_handle, mbuf) != 0) {
            ESP_LOGW(kTag, "Unable to notify Wi-Fi scan data at offset=%zu", offset);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void fail_scan(const char *reason)
{
    std::snprintf(scan_results, sizeof(scan_results), "--ERROR--\t%s\n--END--\n",
                  reason == nullptr ? "unknown" : reason);
    notify_scan_results();
}

int gatt_access(uint16_t, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ctxt->chr != nullptr &&
        ble_uuid_cmp(ctxt->chr->uuid, &kScanResultsUuid.u) == 0) {
        const size_t length = std::strlen(scan_results);
        if (ctxt->offset >= length) return 0;
        const size_t chunk = std::min<size_t>(20, length - ctxt->offset);
        return os_mbuf_append(ctxt->om, scan_results + ctxt->offset, chunk) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ctxt->chr != nullptr &&
        ble_uuid_cmp(ctxt->chr->uuid, &kNetworkStatusUuid.u) == 0) {
        return os_mbuf_append(ctxt->om, network_status, std::strlen(network_status)) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ctxt->chr != nullptr &&
        ble_uuid_cmp(ctxt->chr->uuid, &kBatteryUuid.u) == 0) {
        if (!battery_measurement_valid) return BLE_ATT_ERR_UNLIKELY;
        char measurement[48] = {};
        const int length = format_battery(measurement, sizeof(measurement));
        if (length <= 0) return BLE_ATT_ERR_UNLIKELY;
        if (ctxt->offset >= static_cast<uint16_t>(length)) return 0;
        const size_t chunk = static_cast<size_t>(length) - ctxt->offset;
        return os_mbuf_append(ctxt->om, measurement + ctxt->offset, chunk) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ctxt->chr != nullptr &&
        ble_uuid_cmp(ctxt->chr->uuid, &kColorPresetUuid.u) == 0) {
        if (ctxt->offset >= kColorPresetSize) return 0;
        const size_t chunk = kColorPresetSize - ctxt->offset;
        return os_mbuf_append(ctxt->om, color_preset + ctxt->offset, chunk) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ctxt->chr != nullptr &&
        ble_uuid_cmp(ctxt->chr->uuid, &kFirmwareVersionUuid.u) == 0) {
        const esp_app_desc_t *description = esp_app_get_description();
        const char *version = description == nullptr ? "" : description->version;
        const size_t length = std::strlen(version);
        if (ctxt->offset > length) return BLE_ATT_ERR_INVALID_OFFSET;
        if (ctxt->offset == length) return 0;
        return os_mbuf_append(ctxt->om, version + ctxt->offset, length - ctxt->offset) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ctxt->chr != nullptr &&
        ble_uuid_cmp(ctxt->chr->uuid, &kSerialUuid.u) == 0) {
        const size_t length = std::strlen(serial_number);
        if (ctxt->offset > length) return BLE_ATT_ERR_INVALID_OFFSET;
        if (ctxt->offset == length) return 0;
        return os_mbuf_append(ctxt->om, serial_number + ctxt->offset, length - ctxt->offset) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ctxt->chr != nullptr &&
        ble_uuid_cmp(ctxt->chr->uuid, &kDeviceNameUuid.u) == 0) {
        return os_mbuf_append(ctxt->om, device_name, std::strlen(device_name)) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ctxt->chr != nullptr &&
        ble_uuid_cmp(ctxt->chr->uuid, &kSafeChargingUuid.u) == 0) {
        const uint8_t enabled = safe_charging_mode ? 1 : 0;
        return os_mbuf_append(ctxt->om, &enabled, sizeof(enabled)) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    char *target = nullptr;
    if (attr_handle == 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kSsidUuid.u) == 0) {
        target = pending_ssid;
    } else if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kPasswordUuid.u) == 0) {
        target = pending_password;
    } else if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kApplyUuid.u) == 0) {
        apply_requested = true;
        wake_app_task();
        return 0;
    } else if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kScanRequestUuid.u) == 0) {
        scan_requested = true;
        wake_app_task();
        return 0;
    } else if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kLedCommandUuid.u) == 0) {
        const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
        if (length >= sizeof(pending_led_command) ||
            ble_hs_mbuf_to_flat(ctxt->om, pending_led_command, length, nullptr) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        pending_led_command[length] = '\0';
        led_command_requested = true;
        wake_app_task();
        ESP_LOGI(kTag, "BLE LED command received: %s", pending_led_command);
        return 0;
    } else if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kLedModeUuid.u) == 0) {
        char mode[12] = {};
        const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
        if (length >= sizeof(mode) || ble_hs_mbuf_to_flat(ctxt->om, mode, length, nullptr) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        mode[length] = '\0';
        pending_configuration_mode = std::strcmp(mode, "config") == 0;
        led_mode_command_requested = true;
        wake_app_task();
        return 0;
    } else if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kDeviceNameUuid.u) == 0) {
        const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
        if (length == 0 || length >= sizeof(device_name) ||
            ble_hs_mbuf_to_flat(ctxt->om, pending_device_name, length, nullptr) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        pending_device_name[length] = '\0';
        std::snprintf(device_name, sizeof(device_name), "%s", pending_device_name);
        if (ble_svc_gap_device_name_set(device_name) != 0) return BLE_ATT_ERR_UNLIKELY;
        device_name_requested = true;
        wake_app_task();
        return 0;
    } else if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kSafeChargingUuid.u) == 0) {
        uint8_t enabled = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(enabled) ||
            ble_hs_mbuf_to_flat(ctxt->om, &enabled, sizeof(enabled), nullptr) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        safe_charging_mode = enabled != 0;
        safe_charging_mode_requested = true;
        wake_app_task();
        return 0;
    } else if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kPowerUuid.u) == 0) {
        char command[16] = {};
        const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
        if (length == 0 || length >= sizeof(command) ||
            ble_hs_mbuf_to_flat(ctxt->om, command, length, nullptr) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        command[length] = '\0';
        if (std::strcmp(command, "pause:1") == 0 || std::strcmp(command, "pause:0") == 0) {
            pending_pause_value = command[6] == '1';
            charge_pause_requested = true;
        } else if (std::strcmp(command, "gentle:1") == 0 || std::strcmp(command, "gentle:0") == 0) {
            pending_gentle_value = command[7] == '1';
            gentle_charge_requested = true;
        } else if (std::strcmp(command, "riposo") == 0) {
            shutdown_requested = true;
        } else {
            return BLE_ATT_ERR_UNLIKELY;
        }
        wake_app_task();
        return 0;
    } else if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kColorPresetUuid.u) == 0) {
        const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
        if (length != kColorPresetSize ||
            ble_hs_mbuf_to_flat(ctxt->om, pending_color_preset, length, nullptr) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        color_preset_requested = true;
        wake_app_task();
        return 0;
    } else if (ctxt->chr != nullptr && ble_uuid_cmp(ctxt->chr->uuid, &kFirmwareApproveUuid.u) == 0) {
        const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
        if (length == 0 || length >= sizeof(pending_firmware_version) ||
            ble_hs_mbuf_to_flat(ctxt->om, pending_firmware_version, length, nullptr) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        pending_firmware_version[length] = '\0';
        firmware_approval_requested = true;
        wake_app_task();
        return 0;
    }
    if (target == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length >= kCredentialSize || ble_hs_mbuf_to_flat(ctxt->om, target, length, nullptr) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    target[length] = '\0';
    if (target == pending_ssid || target == pending_password) wake_app_task();
    return 0;
}

void advertise(void)
{
    uint8_t address_type = BLE_OWN_ADDR_PUBLIC;
    int ret = ble_hs_id_infer_auto(0, &address_type);
    if (ret != 0) {
        ESP_LOGE(kTag, "Cannot infer BLE address type: %d", ret);
        return;
    }
    ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<const uint8_t *>(device_name);
    fields.name_len = static_cast<uint8_t>(std::strlen(device_name));
    fields.name_is_complete = 1;
        fields.uuids16 = const_cast<ble_uuid16_t *>(&kServiceUuid);
        fields.num_uuids16 = 1;
        fields.uuids16_is_complete = 1;
    ret = ble_gap_adv_set_fields(&fields);
    if (ret != 0) {
        ESP_LOGE(kTag, "Cannot set BLE advertising fields: %d", ret);
        return;
    }

    ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 1600;
    params.itvl_max = 1920;
    params.itvl_min = 1600;
    params.itvl_max = 1920;
    ret = ble_gap_adv_start(address_type, nullptr, BLE_HS_FOREVER, &params, on_gap_event, nullptr);
    if (ret != 0) {
        ESP_LOGE(kTag, "Cannot start BLE advertising: %d", ret);
    }
}

void on_sync(void)
{
    advertise();
}

int on_gap_event(struct ble_gap_event *event, void *)
{
    if (event == nullptr) {
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        connected = event->connect.status == 0;
        if (connected) {
            scan_conn_handle = event->connect.conn_handle;
            connected_since_us = esp_timer_get_time();
            ESP_LOGI(kTag, "Bluetooth connected");
        } else {
            scan_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            connected_since_us = 0;
            ESP_LOGW(kTag, "Bluetooth connect failed: %d", event->connect.status);
            advertise();
        }
        wake_app_task();
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        connected = false;
        scan_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        connected_since_us = 0;
        ESP_LOGI(kTag, "Bluetooth disconnected, reason=%d", event->disconnect.reason);
        advertise();
        wake_app_task();
    }
    return 0;
}

const ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kServiceUuid.u,
        .characteristics = new ble_gatt_chr_def[] {
            {
                .uuid = &kSsidUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &kPasswordUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &kApplyUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &kScanRequestUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &kScanResultsUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &scan_results_handle,
            },
            {
                .uuid = &kLedCommandUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &kNetworkStatusUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &network_status_handle,
            },
            {
                .uuid = &kLedModeUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &kBatteryUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &battery_handle,
            },
            {
                .uuid = &kDeviceNameUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &kSafeChargingUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &kPowerUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &kColorPresetUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &kSerialUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &kFirmwareVersionUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &kFirmwareApproveUuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {},
        },
    },
    {},
};
}

BluetoothManager::BluetoothManager() : initialized_(false)
{
}

BluetoothManager::~BluetoothManager()
{
    stop();
}

esp_err_t BluetoothManager::begin(const char *initial_device_name, bool initial_safe_charging_mode)
{
    if (initialized_) {
        return ESP_OK;
    }

    app_task_handle = xTaskGetCurrentTaskHandle();
    if (initial_device_name != nullptr && initial_device_name[0] != '\0') {
        std::snprintf(device_name, sizeof(device_name), "%s", initial_device_name);
    }
    safe_charging_mode = initial_safe_charging_mode;
    uint8_t factory_mac[6] = {};
    serial_number[0] = '\0';
    if (esp_efuse_mac_get_default(factory_mac) == ESP_OK) {
        std::snprintf(serial_number, sizeof(serial_number), "%02X:%02X:%02X:%02X:%02X:%02X",
                      factory_mac[0], factory_mac[1], factory_mac[2],
                      factory_mac[3], factory_mac[4], factory_mac[5]);
    } else {
        ESP_LOGE(kTag, "Factory MAC was not read");
    }
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(device_name);
    ble_hs_cfg.sync_cb = on_sync;
    esp_err_t ret = ble_gatts_count_cfg(services);
    if (ret != 0) {
        return ESP_FAIL;
    }
    ret = ble_gatts_add_svcs(services);
    if (ret != 0) {
        return ESP_FAIL;
    }

    active_manager = this;
    nimble_port_freertos_init([](void *) { nimble_port_run(); });
    initialized_ = true;
    ESP_LOGI(kTag, "BLE configuration service enabled");
    return ESP_OK;
}

esp_err_t BluetoothManager::stop()
{
    if (!initialized_) {
        return ESP_OK;
    }
    nimble_port_stop();
    nimble_port_deinit();
    initialized_ = false;
    connected = false;
    connected_since_us = 0;
    scan_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    active_manager = nullptr;
    return ESP_OK;
}

bool BluetoothManager::is_enabled() const
{
    return initialized_;
}

bool BluetoothManager::is_connected() const
{
    return connected;
}

bool BluetoothManager::link_is_held()
{
    if (!connected || scan_conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(scan_conn_handle, &desc) != 0) {
        connected = false;
        scan_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        connected_since_us = 0;
        if (!ble_gap_adv_active()) advertise();
        return false;
    }
    if (connected_since_us == 0) connected_since_us = esp_timer_get_time();
    return esp_timer_get_time() - connected_since_us >= kHeldSessionUs;
}

bool BluetoothManager::take_scan_request()
{
    if (!scan_requested) return false;
    scan_requested = false;
    ESP_LOGI(kTag, "BLE network scan requested");
    return true;
}

void BluetoothManager::set_battery_measurement(uint8_t percentage, uint16_t millivolts,
                                               bool charging, uint8_t charge_status, uint8_t fault,
                                               bool safe_charging, bool charge_paused,
                                               bool gentle_charge, bool estimate_valid)
{
    battery_percentage = percentage > 100 ? 100 : percentage;
    battery_voltage_mv = millivolts;
    battery_charging = charging;
    battery_charge_status = charge_status;
    battery_fault = fault;
    battery_safe = safe_charging;
    battery_paused = charge_paused;
    battery_gentle = gentle_charge;
    battery_estimate_valid = estimate_valid;
    battery_measurement_valid = true;

    char measurement[sizeof(last_battery_notification)] = {};
    const int length = format_battery(measurement, sizeof(measurement));
    if (length <= 0 || std::strcmp(last_battery_notification, measurement) == 0) return;
    std::snprintf(last_battery_notification, sizeof(last_battery_notification), "%s", measurement);
    if (scan_conn_handle == BLE_HS_CONN_HANDLE_NONE || battery_handle == 0) return;
    struct os_mbuf *mbuf = ble_hs_mbuf_from_flat(measurement, static_cast<uint16_t>(length));
    if (mbuf == nullptr || ble_gatts_notify_custom(scan_conn_handle, battery_handle, mbuf) != 0) {
        ESP_LOGW(kTag, "Unable to notify battery status");
    }
}

bool BluetoothManager::take_charge_pause(bool *paused)
{
    if (!charge_pause_requested || paused == nullptr) return false;
    *paused = pending_pause_value;
    charge_pause_requested = false;
    return true;
}

bool BluetoothManager::take_gentle_charge(bool *gentle)
{
    if (!gentle_charge_requested || gentle == nullptr) return false;
    *gentle = pending_gentle_value;
    gentle_charge_requested = false;
    return true;
}

bool BluetoothManager::take_shutdown()
{
    if (!shutdown_requested) return false;
    shutdown_requested = false;
    return true;
}

bool BluetoothManager::take_device_name(char *name, size_t name_size)
{
    if (!device_name_requested || name == nullptr || name_size == 0) return false;
    std::snprintf(name, name_size, "%s", pending_device_name);
    device_name_requested = false;
    return true;
}

bool BluetoothManager::take_safe_charging_mode(bool *enabled)
{
    if (!safe_charging_mode_requested || enabled == nullptr) return false;
    *enabled = safe_charging_mode;
    safe_charging_mode_requested = false;
    return true;
}

void BluetoothManager::scan_networks()
{
    scan_results[0] = '\0';
    wifi_scan_config_t config = {};
    wifi_mode_t original_mode = WIFI_MODE_NULL;
    esp_err_t mode_ret = esp_wifi_get_mode(&original_mode);
    ESP_LOGI(kTag, "Wi-Fi scan starting, mode=%d get_mode=%s",
             original_mode, esp_err_to_name(mode_ret));
    if (mode_ret != ESP_OK) {
        fail_scan(esp_err_to_name(mode_ret));
        return;
    }
    const bool restore_ap_mode = original_mode == WIFI_MODE_AP;
    if (restore_ap_mode) {
        mode_ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        ESP_LOGI(kTag, "Wi-Fi scan mode APSTA: %s", esp_err_to_name(mode_ret));
        if (mode_ret != ESP_OK) {
            fail_scan(esp_err_to_name(mode_ret));
            return;
        }
    }
    config.show_hidden = true;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 0;
    config.scan_time.active.max = 50;
    const esp_err_t scan_ret = esp_wifi_scan_start(&config, true);
    if (scan_ret != ESP_OK) {
        ESP_LOGE(kTag, "Wi-Fi scan failed over BLE: %s", esp_err_to_name(scan_ret));
        if (restore_ap_mode) esp_wifi_set_mode(WIFI_MODE_AP);
        fail_scan(esp_err_to_name(scan_ret));
        return;
    }
    uint16_t count = 0;
    const esp_err_t count_ret = esp_wifi_scan_get_ap_num(&count);
    ESP_LOGI(kTag, "Wi-Fi scan completed: count=%u", count);
    if (count_ret != ESP_OK) {
        ESP_LOGE(kTag, "Wi-Fi scan count failed: %s", esp_err_to_name(count_ret));
        if (restore_ap_mode) esp_wifi_set_mode(WIFI_MODE_AP);
        fail_scan(esp_err_to_name(count_ret));
        return;
    }
    uint16_t records_count = count > 12 ? 12 : count;
    const esp_err_t records_ret = esp_wifi_scan_get_ap_records(&records_count, scan_records);
    if (records_ret != ESP_OK) {
        ESP_LOGE(kTag, "Wi-Fi scan records failed: %s", esp_err_to_name(records_ret));
        if (restore_ap_mode) esp_wifi_set_mode(WIFI_MODE_AP);
        fail_scan(esp_err_to_name(records_ret));
        return;
    }
    size_t used = 0;
    for (uint16_t index = 0; index < records_count && used + 80 < sizeof(scan_results); ++index) {
        const char *ssid = reinterpret_cast<const char *>(scan_records[index].ssid);
        if (ssid[0] == '\0') continue;
        used += std::snprintf(scan_results + used, sizeof(scan_results) - used,
                              "%s\t%d\n", ssid, scan_records[index].rssi);
    }
    if (used + 16 < sizeof(scan_results)) {
        std::snprintf(scan_results + used, sizeof(scan_results) - used, "--END--\n");
    }

    ESP_LOGI(kTag, "Scan payload length=%zu bytes: %s", std::strlen(scan_results), scan_results);

    notify_scan_results();
    if (restore_ap_mode) esp_wifi_set_mode(WIFI_MODE_AP);
    ESP_LOGI(kTag, "Wi-Fi scan over BLE found %u networks", records_count);
}

bool BluetoothManager::take_led_command(char *command, size_t command_size)
{
    if (!led_command_requested || command == nullptr || command_size == 0) return false;
    std::snprintf(command, command_size, "%s", pending_led_command);
    led_command_requested = false;
    return true;
}

void BluetoothManager::set_color_preset(const uint8_t *data, size_t size)
{
    std::memset(color_preset, 0, sizeof(color_preset));
    if (data == nullptr || size == 0) return;
    std::memcpy(color_preset, data, std::min(size, sizeof(color_preset)));
}

bool BluetoothManager::take_color_preset(uint8_t *data, size_t size)
{
    if (!color_preset_requested || data == nullptr || size < kColorPresetSize) return false;
    std::memcpy(color_preset, pending_color_preset, kColorPresetSize);
    std::memcpy(data, pending_color_preset, kColorPresetSize);
    color_preset_requested = false;
    return true;
}

void BluetoothManager::set_network_status(const char *ssid, bool is_connected, const char *ip_address,
                                         bool password_stored)
{
    char updated_status[sizeof(network_status)] = {};
    std::snprintf(updated_status, sizeof(updated_status), "%c\t%s\t%s\t%c",
                  is_connected ? '1' : '0', ssid == nullptr ? "" : ssid,
                  ip_address == nullptr ? "" : ip_address, password_stored ? '1' : '0');
    if (std::strcmp(network_status, updated_status) == 0) return;

    std::snprintf(network_status, sizeof(network_status), "%s", updated_status);
    if (scan_conn_handle == BLE_HS_CONN_HANDLE_NONE || network_status_handle == 0) return;

    struct os_mbuf *mbuf = ble_hs_mbuf_from_flat(network_status, std::strlen(network_status));
    if (mbuf == nullptr || ble_gatts_notify_custom(scan_conn_handle, network_status_handle, mbuf) != 0) {
        ESP_LOGW(kTag, "Unable to notify Wi-Fi connection status");
    }
}

bool BluetoothManager::take_led_mode_command(bool *configuration_mode)
{
    if (!led_mode_command_requested || configuration_mode == nullptr) return false;
    *configuration_mode = pending_configuration_mode;
    led_mode_command_requested = false;
    return true;
}

bool BluetoothManager::take_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    if (!apply_requested || ssid == nullptr || password == nullptr || ssid_size == 0 || password_size == 0) {
        return false;
    }
    std::strncpy(ssid, pending_ssid, ssid_size - 1);
    std::strncpy(password, pending_password, password_size - 1);
    ssid[ssid_size - 1] = '\0';
    password[password_size - 1] = '\0';
    apply_requested = false;
    return true;
}

bool BluetoothManager::take_firmware_approval(char *version, size_t version_size)
{
    if (!firmware_approval_requested || version == nullptr || version_size == 0) return false;
    std::snprintf(version, version_size, "%s", pending_firmware_version);
    firmware_approval_requested = false;
    return true;
}