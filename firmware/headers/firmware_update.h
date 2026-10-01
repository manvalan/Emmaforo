#pragma once

#include "esp_err.h"

const char *firmware_version();
void firmware_update_load();
esp_err_t firmware_update_approve(const char *version);
bool firmware_update_pending();
bool firmware_update_busy();
void firmware_update_poll(bool wifi_connected);
void firmware_update_mark_valid();
