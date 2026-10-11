#include "remote_link.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_he.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "REMOTE";
constexpr char kNamespace[] = "network";
constexpr char kEnabledKey[] = "rl_on";
constexpr char kIntervalKey[] = "rl_min";
constexpr char kBrokerKey[] = "rl_broker";
constexpr char kUserKey[] = "rl_user";
constexpr char kPassKey[] = "rl_pass";
constexpr char kRouterKey[] = "rl_router";
constexpr char kBssidKey[] = "rl_bssid";

// A check-in ends once the queued commands had time to arrive, or after a
// hard limit when the router or the broker cannot be reached.
constexpr int64_t kQuietAfterSubscribeUs = 3LL * 1000000LL;
constexpr int64_t kSessionLimitUs = 30LL * 1000000LL;
constexpr int64_t kTwtAnswerLimitUs = 8LL * 1000000LL;
// Ping the broker rarely: the router keeps the TCP session while the radio
// sleeps between TWT service periods.
constexpr int kMqttKeepaliveS = 240;

constexpr size_t kQueueDepth = 4;
constexpr size_t kCommandSize = 64;
char queue[kQueueDepth][kCommandSize] = {};
size_t queue_head = 0;
size_t queue_count = 0;
portMUX_TYPE queue_lock = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t app_task = nullptr;

void wake_app()
{
    if (app_task != nullptr) xTaskNotifyGive(app_task);
}

const char *mode_name(RemoteLink::Mode mode)
{
    switch (mode) {
    case RemoteLink::Mode::Usb: return "usb";
    case RemoteLink::Mode::Twt: return "twt";
    case RemoteLink::Mode::Interval: return "interval";
    default: return "off";
    }
}

void twt_interval_fields(uint32_t seconds, uint16_t *mantissa, uint8_t *exponent)
{
    // Wake interval = mantissa * 2^exponent microseconds, mantissa <= 65535.
    uint64_t micros = static_cast<uint64_t>(seconds) * 1000000ULL;
    uint8_t exp = 0;
    while ((micros >> exp) > 65535ULL) exp++;
    *exponent = exp;
    *mantissa = static_cast<uint16_t>(micros >> exp);
}
}

void RemoteLink::begin(const char *device_id)
{
    app_task = xTaskGetCurrentTaskHandle();
    std::snprintf(device_id_, sizeof(device_id_), "%s", device_id == nullptr ? "" : device_id);
    std::snprintf(cmd_topic_, sizeof(cmd_topic_), "emmaforo/%s/cmd", device_id_);
    std::snprintf(state_topic_, sizeof(state_topic_), "emmaforo/%s/state", device_id_);
    load();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_ITWT_SETUP, &RemoteLink::wifi_event, this);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_ITWT_TEARDOWN, &RemoteLink::wifi_event, this);
    ESP_LOGI(kTag, "Remote control %s, check-in every %u min, router %s",
             enabled_ ? "on" : "off", interval_minutes_,
             router_ == Router::Twt ? "with TWT" : router_ == Router::Legacy ? "without TWT" : "not probed");
}

void RemoteLink::load()
{
    std::snprintf(broker_, sizeof(broker_), "%s", CONFIG_EMMAFORO_REMOTE_BROKER);
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t value = 0;
    if (nvs_get_u8(handle, kEnabledKey, &value) == ESP_OK) enabled_ = value != 0;
    if (nvs_get_u8(handle, kIntervalKey, &value) == ESP_OK && value >= kMinIntervalMinutes &&
        value <= kMaxIntervalMinutes) {
        interval_minutes_ = value;
    }
    size_t length = sizeof(broker_);
    char broker[sizeof(broker_)] = {};
    if (nvs_get_str(handle, kBrokerKey, broker, &length) == ESP_OK && broker[0] != '\0') {
        std::snprintf(broker_, sizeof(broker_), "%s", broker);
    }
    length = sizeof(user_);
    if (nvs_get_str(handle, kUserKey, user_, &length) != ESP_OK) user_[0] = '\0';
    length = sizeof(pass_);
    if (nvs_get_str(handle, kPassKey, pass_, &length) != ESP_OK) pass_[0] = '\0';
    if (nvs_get_u8(handle, kRouterKey, &value) == ESP_OK && value <= static_cast<uint8_t>(Router::Twt)) {
        router_ = static_cast<Router>(value);
    }
    length = sizeof(router_bssid_);
    if (nvs_get_blob(handle, kBssidKey, router_bssid_, &length) != ESP_OK) router_ = Router::Unknown;
    nvs_close(handle);
}

esp_err_t RemoteLink::save() const
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(handle, kEnabledKey, enabled_ ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_set_u8(handle, kIntervalKey, interval_minutes_);
    if (ret == ESP_OK) ret = nvs_set_str(handle, kBrokerKey, broker_);
    if (ret == ESP_OK) ret = nvs_set_str(handle, kUserKey, user_);
    if (ret == ESP_OK) ret = nvs_set_str(handle, kPassKey, pass_);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

void RemoteLink::save_router() const
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, kRouterKey, static_cast<uint8_t>(router_)) == ESP_OK &&
        nvs_set_blob(handle, kBssidKey, router_bssid_, sizeof(router_bssid_)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

bool RemoteLink::apply_command(const char *command)
{
    if (command == nullptr) return false;
    bool restart_mqtt = false;
    if (std::strcmp(command, "on") == 0) {
        enabled_ = true;
        next_checkin_us_ = 0;
    } else if (std::strcmp(command, "off") == 0) {
        enabled_ = false;
    } else if (std::strncmp(command, "interval:", 9) == 0) {
        const long minutes = std::strtol(command + 9, nullptr, 10);
        if (minutes < kMinIntervalMinutes || minutes > kMaxIntervalMinutes) return false;
        interval_minutes_ = static_cast<uint8_t>(minutes);
    } else if (std::strncmp(command, "broker:", 7) == 0) {
        if (std::strlen(command + 7) >= sizeof(broker_)) return false;
        std::snprintf(broker_, sizeof(broker_), "%s", command[7] != '\0' ? command + 7 : CONFIG_EMMAFORO_REMOTE_BROKER);
        restart_mqtt = true;
    } else if (std::strncmp(command, "user:", 5) == 0) {
        if (std::strlen(command + 5) >= sizeof(user_)) return false;
        std::snprintf(user_, sizeof(user_), "%s", command + 5);
        restart_mqtt = true;
    } else if (std::strncmp(command, "pass:", 5) == 0) {
        if (std::strlen(command + 5) >= sizeof(pass_)) return false;
        std::snprintf(pass_, sizeof(pass_), "%s", command + 5);
        restart_mqtt = true;
    } else {
        return false;
    }
    if (restart_mqtt && mqtt_ != nullptr) {
        stop_mqtt();
        esp_mqtt_client_destroy(mqtt_);
        mqtt_ = nullptr;
    }
    if (save() != ESP_OK) ESP_LOGW(kTag, "Remote settings were not stored");
    ESP_LOGI(kTag, "Remote control %s, check-in every %u min", enabled_ ? "on" : "off", interval_minutes_);
    return true;
}

void RemoteLink::poll(bool power_good, bool wifi_connected, int64_t now_us)
{
    if (!enabled_ || broker_[0] == '\0') {
        if (mqtt_started_) stop_mqtt();
        if (twt_active_) teardown_twt();
        mode_ = Mode::Off;
        wants_wifi_ = false;
        session_active_ = false;
        was_connected_ = wifi_connected;
        was_power_good_ = power_good;
        return;
    }

    // Leaving the charger with a router without TWT: the next check-in is one
    // interval away, not now.
    if (was_power_good_ && !power_good && router_ != Router::Twt) {
        next_checkin_us_ = now_us + static_cast<int64_t>(interval_minutes_) * 60LL * 1000000LL;
        session_active_ = false;
    }
    was_power_good_ = power_good;

    if (wifi_connected && !was_connected_) on_associated(now_us, power_good);
    was_connected_ = wifi_connected;

    // TWT negotiation answers arrive on the Wi-Fi event task.
    if (twt_requested_) {
        const int result = twt_result_;
        if (result != 0 || now_us - twt_requested_us_ >= kTwtAnswerLimitUs) {
            twt_requested_ = false;
            twt_result_ = 0;
            const Router learned = result > 0 ? Router::Twt : Router::Legacy;
            if (learned != router_) {
                router_ = learned;
                save_router();
            }
            ESP_LOGI(kTag, "Router %s individual TWT", result > 0 ? "grants" : "does not grant");
        }
    }

    if (power_good) {
        mode_ = Mode::Usb;
        if (twt_active_) teardown_twt();
        session_active_ = false;
    } else if (router_ == Router::Twt) {
        mode_ = Mode::Twt;
        session_active_ = false;
        if (wifi_connected && !twt_active_ && !twt_requested_) request_twt(now_us);
    } else {
        mode_ = Mode::Interval;
        if (!session_active_ && now_us >= next_checkin_us_) {
            session_active_ = true;
            session_started_us_ = now_us;
            mqtt_subscribed_ = false;
            last_rx_us_ = 0;
            ESP_LOGI(kTag, "Check-in: joining Wi-Fi for queued commands");
        }
    }

    // A TWT router that cannot be reached (the board left home) must not keep
    // the radio retrying: wait one interval, like a check-in, then try again.
    if (mode_ == Mode::Twt && !wifi_connected) {
        if (offline_since_us_ == 0) offline_since_us_ = now_us;
        if (now_us - offline_since_us_ >= kSessionLimitUs && now_us >= twt_retry_us_) {
            twt_retry_us_ = now_us + static_cast<int64_t>(interval_minutes_) * 60LL * 1000000LL;
            offline_since_us_ = 0;
            ESP_LOGI(kTag, "Router not reachable: retry in %u min", interval_minutes_);
        }
    } else {
        offline_since_us_ = 0;
    }
    const bool twt_waiting = mode_ == Mode::Twt && now_us < twt_retry_us_;
    const bool connected_mode = mode_ == Mode::Usb || (mode_ == Mode::Twt && !twt_waiting);
    wants_wifi_ = connected_mode || session_active_;

    if (wifi_connected && wants_wifi_) {
        if (!mqtt_started_) start_mqtt();
    } else if (mqtt_started_ && !wifi_connected) {
        stop_mqtt();
    }

    if (session_active_) {
        const int64_t last = last_rx_us_ > 0 ? last_rx_us_ : 0;
        const bool probing = router_ == Router::Unknown || twt_requested_;
        const bool quiet = mqtt_subscribed_ && !state_dirty_ && !probing &&
                           now_us - last >= kQuietAfterSubscribeUs;
        if (quiet || now_us - session_started_us_ >= kSessionLimitUs) end_session(now_us);
    }
}

void RemoteLink::on_associated(int64_t now_us, bool power_good)
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    if (std::memcmp(ap.bssid, router_bssid_, sizeof(router_bssid_)) != 0) {
        std::memcpy(router_bssid_, ap.bssid, sizeof(router_bssid_));
        router_ = Router::Unknown;
        ESP_LOGI(kTag, "New router: TWT support will be probed");
    }
    if (router_ != Router::Unknown || power_good) return;
    wifi_phy_mode_t phy = WIFI_PHY_MODE_LR;
    if (esp_wifi_sta_get_negotiated_phymode(&phy) != ESP_OK || phy != WIFI_PHY_MODE_HE20) {
        router_ = Router::Legacy;
        save_router();
        ESP_LOGI(kTag, "Router is not Wi-Fi 6: check-in every %u min", interval_minutes_);
        return;
    }
    request_twt(now_us);
}

void RemoteLink::request_twt(int64_t now_us)
{
    wifi_twt_config_t twt_config = {};
    twt_config.post_wakeup_event = false;
    twt_config.twt_enable_keep_alive = true;
    esp_wifi_sta_twt_config(&twt_config);

    uint16_t mantissa = 0;
    uint8_t exponent = 0;
    twt_interval_fields(CONFIG_EMMAFORO_REMOTE_TWT_INTERVAL_S, &mantissa, &exponent);
    wifi_itwt_setup_config_t setup = {};
    setup.setup_cmd = TWT_REQUEST;
    setup.trigger = 1;
    setup.flow_type = 0;
    setup.flow_id = 0;
    setup.wake_invl_expn = exponent;
    setup.wake_duration_unit = 0;
    setup.min_wake_dura = 255;  // 255 x 256 us, about 65 ms awake per period
    setup.wake_invl_mant = mantissa;
    setup.twt_id = 0;
    setup.timeout_time_ms = 5000;
    twt_result_ = 0;
    if (esp_wifi_sta_itwt_setup(&setup) == ESP_OK) {
        twt_requested_ = true;
        twt_requested_us_ = now_us;
    } else {
        router_ = Router::Legacy;
        save_router();
        ESP_LOGI(kTag, "TWT request not sent: check-in every %u min", interval_minutes_);
    }
}

void RemoteLink::teardown_twt()
{
    esp_wifi_sta_itwt_teardown(twt_flow_id_);
    twt_active_ = false;
}

void RemoteLink::end_session(int64_t now_us)
{
    if (mqtt_started_) stop_mqtt();
    session_active_ = false;
    next_checkin_us_ = now_us + static_cast<int64_t>(interval_minutes_) * 60LL * 1000000LL;
    ESP_LOGI(kTag, "Check-in done: next in %u min", interval_minutes_);
}

void RemoteLink::start_mqtt()
{
    if (mqtt_ == nullptr) {
        esp_mqtt_client_config_t config = {};
        config.broker.address.uri = broker_;
        if (std::strncmp(broker_, "mqtts://", 8) == 0) {
            config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
        static char client_id[32] = {};
        std::snprintf(client_id, sizeof(client_id), "emmaforo-%s", device_id_);
        config.credentials.client_id = client_id;
        if (user_[0] != '\0') config.credentials.username = user_;
        if (pass_[0] != '\0') config.credentials.authentication.password = pass_;
        // Persistent session: QoS 1 commands wait at the broker while the board is away.
        config.session.disable_clean_session = true;
        config.session.keepalive = kMqttKeepaliveS;
        mqtt_ = esp_mqtt_client_init(&config);
        if (mqtt_ == nullptr) {
            ESP_LOGW(kTag, "MQTT client not created");
            return;
        }
        esp_mqtt_client_register_event(mqtt_, MQTT_EVENT_ANY, &RemoteLink::mqtt_event, this);
    }
    mqtt_subscribed_ = false;
    if (esp_mqtt_client_start(mqtt_) == ESP_OK) {
        mqtt_started_ = true;
    } else {
        ESP_LOGW(kTag, "MQTT client did not start");
    }
}

void RemoteLink::stop_mqtt()
{
    if (mqtt_ != nullptr && mqtt_started_) {
        if (mqtt_connected_) esp_mqtt_client_disconnect(mqtt_);
        esp_mqtt_client_stop(mqtt_);
    }
    mqtt_started_ = false;
    mqtt_connected_ = false;
    mqtt_subscribed_ = false;
}

uint32_t RemoteLink::next_wake_ms(int64_t now_us) const
{
    if (!enabled_ || session_active_) return UINT32_MAX;
    int64_t due = 0;
    if (mode_ == Mode::Interval) {
        due = next_checkin_us_;
    } else if (mode_ == Mode::Twt && twt_retry_us_ > now_us) {
        due = twt_retry_us_;
    } else {
        return UINT32_MAX;
    }
    if (due <= now_us) return 0;
    const int64_t ms = (due - now_us) / 1000;
    return ms > static_cast<int64_t>(UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(ms);
}

bool RemoteLink::take_command(char *out, size_t size)
{
    if (out == nullptr || size == 0) return false;
    bool taken = false;
    taskENTER_CRITICAL(&queue_lock);
    if (queue_count > 0) {
        std::snprintf(out, size, "%s", queue[queue_head]);
        queue_head = (queue_head + 1) % kQueueDepth;
        queue_count--;
        taken = true;
    }
    taskEXIT_CRITICAL(&queue_lock);
    return taken;
}

void RemoteLink::publish_state(const char *json)
{
    if (json == nullptr) return;
    if (std::strcmp(json, state_) != 0) {
        std::snprintf(state_, sizeof(state_), "%s", json);
        state_dirty_ = true;
    }
    if (state_dirty_ && mqtt_connected_ && mqtt_ != nullptr) {
        if (esp_mqtt_client_publish(mqtt_, state_topic_, state_, 0, 1, 1) >= 0) state_dirty_ = false;
    }
}

void RemoteLink::status_text(char *out, size_t size) const
{
    if (out == nullptr || size == 0) return;
    // format, enabled, interval min, mode, router (0 unknown, 1 no TWT, 2 TWT), broker connected
    std::snprintf(out, size, "1\t%u\t%u\t%s\t%u\t%u", enabled_ ? 1 : 0, interval_minutes_, mode_name(mode_),
                  static_cast<unsigned>(router_), mqtt_connected_ ? 1 : 0);
}

void RemoteLink::wifi_event(void *arg, esp_event_base_t, int32_t id, void *data)
{
    auto *self = static_cast<RemoteLink *>(arg);
    if (self == nullptr || data == nullptr) return;
    if (id == WIFI_EVENT_ITWT_SETUP) {
        const auto *setup = static_cast<wifi_event_sta_itwt_setup_t *>(data);
        // The driver reports 1 for an accepted agreement (see the IDF itwt example).
        if (setup->status == 1) {
            self->twt_flow_id_ = setup->config.flow_id;
            self->twt_active_ = true;
            self->twt_result_ = 1;
        } else {
            self->twt_result_ = -1;
        }
    } else if (id == WIFI_EVENT_ITWT_TEARDOWN) {
        self->twt_active_ = false;
    }
    wake_app();
}

void RemoteLink::mqtt_event(void *arg, esp_event_base_t, int32_t id, void *data)
{
    auto *self = static_cast<RemoteLink *>(arg);
    auto *event = static_cast<esp_mqtt_event_handle_t>(data);
    if (self == nullptr || event == nullptr) return;
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
        self->mqtt_connected_ = true;
        self->state_dirty_ = true;
        esp_mqtt_client_subscribe(event->client, self->cmd_topic_, 1);
        ESP_LOGI(kTag, "Broker connected");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        self->mqtt_subscribed_ = true;
        self->last_rx_us_ = esp_timer_get_time();
        break;
    case MQTT_EVENT_DISCONNECTED:
        self->mqtt_connected_ = false;
        self->mqtt_subscribed_ = false;
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len == static_cast<int>(std::strlen(self->cmd_topic_)) &&
            std::strncmp(event->topic, self->cmd_topic_, event->topic_len) == 0 &&
            event->data_len > 0 && event->data_len < static_cast<int>(kCommandSize) &&
            event->current_data_offset == 0 && event->total_data_len == event->data_len) {
            taskENTER_CRITICAL(&queue_lock);
            const size_t slot = (queue_head + queue_count) % kQueueDepth;
            std::memcpy(queue[slot], event->data, event->data_len);
            queue[slot][event->data_len] = '\0';
            if (queue_count < kQueueDepth) {
                queue_count++;
            } else {
                queue_head = (queue_head + 1) % kQueueDepth;  // drop the oldest
            }
            taskEXIT_CRITICAL(&queue_lock);
            self->last_rx_us_ = esp_timer_get_time();
        }
        break;
    default:
        break;
    }
    wake_app();
}
