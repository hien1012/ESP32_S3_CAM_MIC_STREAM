/**
 * ws_client.c
 * WebSocket client – sends binary video & audio frames to PC server.
 *
 * Protocol header (7 bytes, big-endian):
 *   [0]   TYPE:        0x01 = VIDEO (GRAY8 raw), 0x02 = AUDIO (int16 PCM)
 *   [1-2] reserved:    0x00 0x00
 *   [3-6] PAYLOAD_LEN: uint32_t big-endian
 *
 * A mutex serialises concurrent sends from the camera (Core1) and
 * microphone (Core0) tasks.
 */

#include "ws_client.h"
#include "app_config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "WS_CLIENT";

static esp_websocket_client_handle_t s_client     = NULL;
static EventGroupHandle_t            s_evt_group  = NULL;
static SemaphoreHandle_t             s_send_mutex = NULL;

#define WS_CONNECTED_BIT  BIT0

/* ── Event handler ──────────────────────────────────────────────────────── */
static void ws_event_handler(void *handler_args,
                              esp_event_base_t base,
                              int32_t event_id,
                              void *event_data)
{
    switch ((esp_websocket_event_id_t)event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to server");
        xEventGroupSetBits(s_evt_group, WS_CONNECTED_BIT);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from server – will auto-reconnect");
        xEventGroupClearBits(s_evt_group, WS_CONNECTED_BIT);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;
    default:
        break;
    }
}

/* ── Internal send ──────────────────────────────────────────────────────── */
static esp_err_t send_binary_msg(uint8_t type, const uint8_t *data, size_t len)
{
    if (!ws_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Build header + payload in one contiguous buffer so the client
     * sends a single WebSocket binary frame. */
    size_t   total = 7 + len;
    uint8_t *buf   = malloc(total);
    if (!buf) {
        ESP_LOGE(TAG, "malloc(%zu) failed", total);
        return ESP_ERR_NO_MEM;
    }

    buf[0] = type;
    buf[1] = 0x00;
    buf[2] = 0x00;
    buf[3] = (uint8_t)((len >> 24) & 0xFF);
    buf[4] = (uint8_t)((len >> 16) & 0xFF);
    buf[5] = (uint8_t)((len >>  8) & 0xFF);
    buf[6] = (uint8_t)( len        & 0xFF);
    memcpy(buf + 7, data, len);

    /* Serialise concurrent sends */
    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    int sent = esp_websocket_client_send_bin(s_client, (const char *)buf,
                                             (int)total, pdMS_TO_TICKS(2000));
    xSemaphoreGive(s_send_mutex);

    free(buf);

    if (sent < 0) {
        ESP_LOGW(TAG, "send_bin failed (ret=%d)", sent);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t ws_client_start(const char *device_id)
{
    s_evt_group  = xEventGroupCreate();
    s_send_mutex = xSemaphoreCreateMutex();

    if (!s_evt_group || !s_send_mutex) {
        return ESP_ERR_NO_MEM;
    }

    /* Build WebSocket URI: ws://<IP>:<PORT><PATH><device_id> */
    char uri[160];
    snprintf(uri, sizeof(uri), "ws://%s:%d%s%s",
             APP_SERVER_IP, APP_SERVER_PORT,
             APP_SERVER_WS_PATH, device_id);
    ESP_LOGI(TAG, "Connecting to %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri                    = uri,
        .reconnect_timeout_ms   = APP_UPLOAD_RETRY_MS,
        .network_timeout_ms     = 10000,
        .ping_interval_sec      = 10,
        /* Increase buffer for 307 KB video frames */
        .buffer_size            = 320 * 1024,
        .transport              = WEBSOCKET_TRANSPORT_OVER_TCP,
    };

    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);
    esp_websocket_client_start(s_client);

    return ESP_OK;
}

bool ws_client_is_connected(void)
{
    if (!s_evt_group) return false;
    return (xEventGroupGetBits(s_evt_group) & WS_CONNECTED_BIT) != 0;
}

esp_err_t ws_client_send_video(const uint8_t *data, size_t len)
{
    return send_binary_msg(0x01, data, len);
}

esp_err_t ws_client_send_audio(const uint8_t *data, size_t len)
{
    return send_binary_msg(0x02, data, len);
}

void ws_client_stop(void)
{
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    if (s_evt_group) {
        vEventGroupDelete(s_evt_group);
        s_evt_group = NULL;
    }
    if (s_send_mutex) {
        vSemaphoreDelete(s_send_mutex);
        s_send_mutex = NULL;
    }
}
