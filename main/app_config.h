#pragma once

#include "sdkconfig.h"

/* ── WiFi ───────────────────────────────────────────────── */
#define APP_WIFI_SSID        CONFIG_FIELD_WIFI_SSID
#define APP_WIFI_PASSWORD    CONFIG_FIELD_WIFI_PASSWORD

/* ── PC Server ──────────────────────────────────────────── */
#define APP_SERVER_IP        CONFIG_FIELD_SERVER_IP
#define APP_SERVER_PORT      8765
#define APP_SERVER_WS_PATH   "/ws/node/"   /* device_id appended at runtime */

/* ── Device ──────────────────────────────────────────────── */
#define APP_DEVICE_ROLE      "cam"          /* prefix for MAC-based device ID */

/* ── Retry ───────────────────────────────────────────────── */
#define APP_UPLOAD_RETRY_MS  5000
