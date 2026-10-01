idf_component_register(
    SRCS "emmaforo.cc" "../src/serial_rgb_led.cpp" "../src/bq25896.cpp" "../src/battery_status.cpp" "../src/wifi_manager.cpp" "../src/bluetooth_manager.cpp" "../src/settings_store.cpp" "../src/setup_portal.cpp" "../src/firmware_update.cpp"
    INCLUDE_DIRS "." "../headers"
    REQUIRES bt driver esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_hw_support mdns esp_https_ota app_update esp_http_client mbedtls esp_timer esp_pm
)
