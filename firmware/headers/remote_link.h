#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mqtt_client.h"

// Remote control over the stored Wi-Fi network, off unless the user turns it
// on. How the radio is used depends on power and on the router in front:
//
//   Usb       on the charger: Wi-Fi stays associated, commands arrive at once.
//   Twt       on battery with a Wi-Fi 6 router that grants an individual
//             Target Wake Time agreement: Wi-Fi stays associated but the radio
//             sleeps between service periods, commands arrive within seconds.
//   Interval  on battery with any other router: Wi-Fi joins every N minutes,
//             collects the commands the broker queued and turns off again.
//
// The router is probed the first time the board joins it on battery, and the
// result is kept in NVS with its BSSID, so a different router is probed again.
// Commands and state go through an MQTT broker with a persistent session, so
// commands sent while the board sleeps are delivered at the next check-in.
//
// Privacy: no account and no hardware identifier. The phone creates a random
// key and writes it over Bluetooth; topic names are derived from it and every
// payload is sealed with it (remote_crypto.h), so the broker only sees random
// topics and ciphertext. Remote control cannot be turned on without a key.
class RemoteLink {
public:
    enum class Mode : uint8_t { Off, Usb, Twt, Interval };
    enum class Router : uint8_t { Unknown, Legacy, Twt };

    static constexpr uint8_t kDefaultIntervalMinutes = 10;
    static constexpr uint8_t kMinIntervalMinutes = 1;
    static constexpr uint8_t kMaxIntervalMinutes = 60;

    void begin();
    bool enabled() const { return enabled_; }
    uint8_t interval_minutes() const { return interval_minutes_; }

    // Text commands from Bluetooth: "key:<64 hex>", "forget", "on", "off",
    // "interval:<minutes>", "broker:<uri>", "user:<name>", "pass:<secret>".
    // Stored in NVS. The key can be written but never read back.
    bool apply_command(const char *command);

    // Mode is published as its number: 0 off, 1 usb, 2 twt, 3 interval.
    // Called once per main loop with the latest charger power state and
    // whether the station is associated with an IP address.
    void poll(bool power_good, bool wifi_connected, int64_t now_us);
    bool wants_wifi() const { return wants_wifi_; }
    bool session_active() const { return session_active_; }
    // Milliseconds until poll() has something to do on its own, or UINT32_MAX.
    uint32_t next_wake_ms(int64_t now_us) const;
    Mode mode() const { return mode_; }
    Router router() const { return router_; }

    // A command received from the broker: the same text the app writes over
    // Bluetooth to FFF6 (lamp) or FFFA (pause:/gentle:/riposo).
    bool take_command(char *out, size_t size);
    // Retained state for the server. Published only when it changed.
    void publish_state(const char *json);
    void status_text(char *out, size_t size) const;

private:
    static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data);
    static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data);
    void load();
    void derive_keys();
    void save_sequence(uint64_t sequence) const;
    esp_err_t save() const;
    void save_router() const;
    void start_mqtt();
    void stop_mqtt();
    void request_twt(int64_t now_us);
    void teardown_twt();
    void on_associated(int64_t now_us, bool power_good);
    void end_session(int64_t now_us);

    bool enabled_ = false;
    uint8_t interval_minutes_ = kDefaultIntervalMinutes;
    char broker_[128] = {};
    char user_[64] = {};
    char pass_[64] = {};
    uint8_t key_[32] = {};
    bool has_key_ = false;
    uint8_t command_key_[32] = {};
    uint8_t state_key_[32] = {};
    char topic_id_[33] = {};
    char client_id_[24] = {};
    char cmd_topic_[56] = {};
    char state_topic_[56] = {};
    volatile uint64_t last_sequence_ = 0;

    Mode mode_ = Mode::Off;
    Router router_ = Router::Unknown;
    uint8_t router_bssid_[6] = {};
    bool wants_wifi_ = false;
    bool was_connected_ = false;
    bool was_power_good_ = false;

    bool session_active_ = false;
    int64_t session_started_us_ = 0;
    int64_t next_checkin_us_ = 0;
    int64_t offline_since_us_ = 0;
    int64_t twt_retry_us_ = 0;

    bool twt_requested_ = false;
    int64_t twt_requested_us_ = 0;
    volatile bool twt_active_ = false;
    volatile int twt_result_ = 0;  // 0 none, 1 success, -1 failed
    volatile uint8_t twt_flow_id_ = 0;

    esp_mqtt_client_handle_t mqtt_ = nullptr;
    bool mqtt_started_ = false;
    volatile bool mqtt_connected_ = false;
    volatile bool mqtt_subscribed_ = false;
    volatile int64_t last_rx_us_ = 0;
    volatile bool state_dirty_ = true;
    char state_[192] = {};
};
