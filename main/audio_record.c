/**
 * audio_record.c
 * I2S / PDM microphone capture task – streams int16 PCM over WebSocket.
 *
 * Switch between modes with the I2S_MIC define:
 *   #define I2S_MIC 1   → I2S Standard (INMP441 / MSM261S4030H0R, 3 wires: SCK, WS, SD)
 *   #define I2S_MIC 0   → PDM (2 wires: CLK, DATA)
 *
 * Audio format: 16-bit signed PCM, 16 kHz, mono
 * WebSocket payload: 1024 samples = 2048 bytes per message
 */

#include "audio_record.h"
#include "ws_client.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Mode selection ─────────────────────────────────────────────────────────
 *  1 = I2S Standard (INMP441 / similar): BCLK + WS + DATA (3 pins)
 *  0 = PDM: CLK + DATA (2 pins)
 */
#define I2S_MIC  1

#if I2S_MIC
#include "driver/i2s_std.h"
#else
#include "driver/i2s_pdm.h"
#endif

static const char *TAG = "AUDIO_REC";

/* ── GPIO pins ──────────────────────────────────────────────────────────── */
#if I2S_MIC
#define MIC_BCLK_IO   GPIO_NUM_5   /* SCK / BCLK */
#define MIC_WS_IO     GPIO_NUM_4   /* WS  / LRCLK */
#define MIC_DIN_IO    GPIO_NUM_6   /* SD  / DOUT from mic */
#else
#define PDM_CLK_IO    GPIO_NUM_26
#define PDM_DIN_IO    GPIO_NUM_25
#endif

/* ── Audio parameters ───────────────────────────────────────────────────── */
#define SAMPLE_RATE         16000
#define DMA_DESC_NUM        8
#define DMA_FRAME_NUM       512
#define RAW_READ_BYTES      8192   /* bytes per i2s_channel_read call (= 2048 × uint32_t) */
#define PCM_BLOCK_SAMPLES   1024   /* samples per WebSocket message */
#define PCM_SHIFT_BITS      16     /* right-shift raw 32-bit word to get 16-bit signed */
#define PCM_GAIN            30     /* linear gain applied after shift */
#define FLUSH_READS         4      /* DMA warmup reads to discard */

static i2s_chan_handle_t s_rx_handle = NULL;
static TaskHandle_t      s_mic_task_handle = NULL;

/* ── Capture task ───────────────────────────────────────────────────────── */
static void mic_stream_task(void *pvParameters)
{
    /* Raw DMA buffer: RAW_READ_BYTES / 4 = 2048 uint32_t words */
    static uint32_t raw_buf[RAW_READ_BYTES / 4];
    /* Accumulation buffer: one WebSocket payload */
    static int16_t  pcm_block[PCM_BLOCK_SAMPLES];
    int pcm_idx = 0;

    while (true) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_handle, raw_buf,
                                         sizeof(raw_buf), &bytes_read,
                                         pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read error: %s", esp_err_to_name(err));
            continue;
        }

        int n_words = (int)(bytes_read / 4);

        for (int i = 0; i < n_words; i++) {
            /* Convert 32-bit I2S word to 16-bit signed PCM with gain */
            int32_t val = (int32_t)raw_buf[i] >> PCM_SHIFT_BITS;
            val *= PCM_GAIN;
            /* Clamp */
            if (val >  32767) val =  32767;
            if (val < -32768) val = -32768;

            pcm_block[pcm_idx++] = (int16_t)val;

            if (pcm_idx >= PCM_BLOCK_SAMPLES) {
                if (ws_client_is_connected()) {
                    esp_err_t serr = ws_client_send_audio(
                        (const uint8_t *)pcm_block,
                        PCM_BLOCK_SAMPLES * sizeof(int16_t));
                    if (serr != ESP_OK) {
                        ESP_LOGW(TAG, "ws_client_send_audio: %s",
                                 esp_err_to_name(serr));
                    }
                }
                pcm_idx = 0;
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t audio_record_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, NULL, &s_rx_handle),
        TAG, "i2s_new_channel failed");

#if I2S_MIC
    /* ── I2S Standard (e.g. INMP441) ─────────────────────────────────── */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK_IO,
            .ws   = MIC_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_DIN_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_rx_handle, &std_cfg),
        TAG, "i2s_channel_init_std_mode failed");

    ESP_LOGI(TAG, "Mic mode: I2S Standard  BCLK=%d WS=%d DIN=%d",
             MIC_BCLK_IO, MIC_WS_IO, MIC_DIN_IO);
#else
    /* ── PDM ────────────────────────────────────────────────────────────── */
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = PDM_CLK_IO,
            .din = PDM_DIN_IO,
            .invert_flags = { .clk_inv = false },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_pdm_rx_mode(s_rx_handle, &pdm_cfg),
        TAG, "i2s_channel_init_pdm_rx_mode failed");

    ESP_LOGI(TAG, "Mic mode: PDM  CLK=%d DIN=%d", PDM_CLK_IO, PDM_DIN_IO);
#endif

    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_rx_handle),
        TAG, "i2s_channel_enable failed");

    /* Flush warmup (clear DMA pipeline residue) */
    {
        static uint32_t flush_buf[RAW_READ_BYTES / 4];
        size_t dummy;
        for (int i = 0; i < FLUSH_READS; i++) {
            i2s_channel_read(s_rx_handle, flush_buf,
                             sizeof(flush_buf), &dummy, pdMS_TO_TICKS(100));
        }
    }

    ESP_LOGI(TAG, "Audio ready: %d Hz, 16-bit, mono, gain×%d, block=%d samples",
             SAMPLE_RATE, PCM_GAIN, PCM_BLOCK_SAMPLES);
    return ESP_OK;
}

void audio_stream_start(void)
{
    if (s_mic_task_handle != NULL) {
        ESP_LOGW(TAG, "audio_stream_start called but task already running");
        return;
    }
    xTaskCreatePinnedToCore(
        mic_stream_task, "mic_cap",
        4096, NULL,
        5,                   /* Priority 5 – lower than cam task */
        &s_mic_task_handle,
        1                    /* Core 0 */
    );
    ESP_LOGI(TAG, "Audio stream task started on Core 0");
}

void audio_stream_stop(void)
{
    if (s_mic_task_handle) {
        vTaskDelete(s_mic_task_handle);
        s_mic_task_handle = NULL;
        ESP_LOGI(TAG, "Audio stream task stopped");
    }
    if (s_rx_handle) {
        i2s_channel_disable(s_rx_handle);
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }
}
