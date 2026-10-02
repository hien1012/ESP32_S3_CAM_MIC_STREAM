/**
 * cam_record.c
 * DVP Camera capture task – streams GRAY8 raw frames over WebSocket.
 *
 * Supported sensors: OV2640, OV7670, OV7725
 *
 * OV2640 XCLK note:
 *   Some OV2640 modules include an onboard 12 MHz oscillator on XVCLK.
 *   If yours does, set USE_OV2640_ONBOARD_XVCLK to 1 – the ESP32 will
 *   NOT drive the XCLK pin and the two clock sources will not conflict.
 *   OV7670 / OV7725 always need XCLK from the ESP32 (leave at 0).
 */

#include "cam_record.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor.h"   /* OV2640_PID etc. */

#include "camera_pinout.h"
#include "ws_client.h"

static const char *TAG = "CAM_REC";

/* ── XCLK source selection ──────────────────────────────────────────────────
 *  0 : ESP32 generates 16 MHz XCLK (required for OV7670 / OV7725)
 *  1 : OV2640 onboard 12 MHz oscillator drives XVCLK; ESP32 pin is Hi-Z
 */
#define USE_OV2640_ONBOARD_XVCLK  0

/* Target frame interval in ms (5 fps → 200 ms) */
#define FRAME_INTERVAL_MS  (1000U / CAM_TARGET_FPS)

/* ── Task ───────────────────────────────────────────────────────────────── */
static TaskHandle_t s_cam_task_handle = NULL;

static void cam_capture_task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    int64_t stats_start_us = esp_timer_get_time();
    uint32_t stats_frames = 0;
    uint64_t stats_capture_us = 0;
    uint64_t stats_send_us = 0;

    while (true) {
        int64_t capture_start_us = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        int64_t capture_done_us = esp_timer_get_time();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (ws_client_is_connected()) {
            int64_t send_start_us = esp_timer_get_time();
            esp_err_t err = ws_client_send_video(fb->buf, fb->len);
            stats_send_us += (uint64_t)(esp_timer_get_time() - send_start_us);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ws_client_send_video: %s", esp_err_to_name(err));
            }
        }

        stats_capture_us += (uint64_t)(capture_done_us - capture_start_us);
        stats_frames++;
        if (stats_frames == 10) {
            int64_t now_us = esp_timer_get_time();
            uint32_t elapsed_ms = (uint32_t)((now_us - stats_start_us) / 1000);
            uint32_t fps_x10 = elapsed_ms ? (stats_frames * 10000U) / elapsed_ms : 0;
            ESP_LOGI(TAG, "Stream stats: fps=%u.%u, capture_avg=%u ms, send_avg=%u ms, frame=%u bytes",
                     (unsigned)(fps_x10 / 10), (unsigned)(fps_x10 % 10),
                     (unsigned)(stats_capture_us / stats_frames / 1000),
                     (unsigned)(stats_send_us / stats_frames / 1000),
                     (unsigned)fb->len);
            stats_start_us = now_us;
            stats_frames = 0;
            stats_capture_us = 0;
            stats_send_us = 0;
        }

        esp_camera_fb_return(fb);

        /* Pace to target FPS; vTaskDelayUntil absorbs processing time */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FRAME_INTERVAL_MS));
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t cam_record_init(void)
{
    camera_config_t config = {
        .pin_pwdn      = CAM_PIN_PWDN,
        .pin_reset     = CAM_PIN_RESET,
#if USE_OV2640_ONBOARD_XVCLK
        .pin_xclk      = -1,               /* Hi-Z; OV2640 oscillator drives XVCLK */
        .xclk_freq_hz  = 20000000,         /* hint to driver (>10 MHz → parallel I2S mode) */
#else
        .pin_xclk      = CAM_PIN_XCLK,
        .xclk_freq_hz  = 16000000,
#endif
        .pin_sccb_sda  = CAM_PIN_SIOD,
        .pin_sccb_scl  = CAM_PIN_SIOC,
        .pin_d7        = CAM_PIN_D7,
        .pin_d6        = CAM_PIN_D6,
        .pin_d5        = CAM_PIN_D5,
        .pin_d4        = CAM_PIN_D4,
        .pin_d3        = CAM_PIN_D3,
        .pin_d2        = CAM_PIN_D2,
        .pin_d1        = CAM_PIN_D1,
        .pin_d0        = CAM_PIN_D0,
        .pin_vsync     = CAM_PIN_VSYNC,
        .pin_href      = CAM_PIN_HREF,
        .pin_pclk      = CAM_PIN_PCLK,
        .ledc_timer    = LEDC_TIMER_0,
        .ledc_channel  = LEDC_CHANNEL_0,
        .pixel_format  = PIXFORMAT_GRAYSCALE,
        .frame_size    = FRAMESIZE_VGA,        /* 640 × 480 */
        .jpeg_quality  = 12,                   /* unused for GRAYSCALE */
        .fb_count      = 5,                    /* limit PSRAM use to two frames */
        .fb_location   = CAMERA_FB_IN_PSRAM,
        .grab_mode     = CAMERA_GRAB_LATEST,   /* always newest frame */
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
        return err;
    }

    /* Sensor PID check */
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "esp_camera_sensor_get returned NULL");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sensor PID = 0x%04X", s->id.PID);

    if (s->id.PID == OV2640_PID && !USE_OV2640_ONBOARD_XVCLK) {
        ESP_LOGW(TAG, "OV2640 detected with ESP32-driven XCLK. "
                      "If your module has an onboard 12 MHz oscillator, "
                      "set USE_OV2640_ONBOARD_XVCLK=1.");
    } else if (s->id.PID != OV2640_PID && USE_OV2640_ONBOARD_XVCLK) {
        ESP_LOGE(TAG, "Non-OV2640 sensor detected (0x%04X) but XCLK output "
                      "is disabled (USE_OV2640_ONBOARD_XVCLK=1). Camera will "
                      "likely not work. Set USE_OV2640_ONBOARD_XVCLK=0.",
                 s->id.PID);
    }

    /* Warmup: discard first 10 frames to let AEC/AGC settle */
    ESP_LOGI(TAG, "Camera warmup (10 frames)...");
    for (int i = 0; i < 10; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
    }

    ESP_LOGI(TAG, "Camera ready: %u x %u GRAY8 @ %u fps",
             CAM_IMAGE_WIDTH, CAM_IMAGE_HEIGHT, CAM_TARGET_FPS);
    return ESP_OK;
}

void cam_stream_start(void)
{
    if (s_cam_task_handle != NULL) {
        ESP_LOGW(TAG, "cam_stream_start called but task already running");
        return;
    }
    xTaskCreatePinnedToCore(
        cam_capture_task, "cam_cap",
        4096, NULL,
        6,                  /* Priority 6 – higher than mic task */
        &s_cam_task_handle,
        1                   /* Core 1 */
    );
    ESP_LOGI(TAG, "Camera stream task started on Core 1");
}

void cam_stream_stop(void)
{
    if (s_cam_task_handle) {
        vTaskDelete(s_cam_task_handle);
        s_cam_task_handle = NULL;
        ESP_LOGI(TAG, "Camera stream task stopped");
    }
}
