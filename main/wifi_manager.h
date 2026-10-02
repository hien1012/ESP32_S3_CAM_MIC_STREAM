#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

esp_err_t wifi_manager_init(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_wait_connected(TickType_t timeout_ticks);
