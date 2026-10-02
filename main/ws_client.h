#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

esp_err_t ws_client_start(const char *device_id);
esp_err_t ws_client_send_video(const uint8_t *data, size_t len);
esp_err_t ws_client_send_audio(const uint8_t *data, size_t len);
bool ws_client_is_connected(void);
void ws_client_stop(void);
