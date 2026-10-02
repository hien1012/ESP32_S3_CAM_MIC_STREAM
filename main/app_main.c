#include "nvs_flash.h"
#include "device_identity.h"
#include "wifi_manager.h"
#include "cam_record.h"
#include "audio_record.h"
#include "ws_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
static const char *TAG = "MAIN";

#define BLINK_GPIO 48 


static led_strip_handle_t led_strip;

esp_err_t turn_off_led(){
    /* LED strip initialization configuration */
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1, // Number of onboard LEDs
    };

    /* RMT backend configuration */
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };

    /* Create the LED strip object using RMT backend */
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_LOGI(TAG, "LED strip initialized.");

    /*Turn OFF immediately by setting RGB to 0,0,0 */
    led_strip_set_pixel(led_strip, 0, 0, 0, 0);
    led_strip_refresh(led_strip);
    ESP_LOGI(TAG, "LED turned off.");
    return ESP_OK;
}

void app_main(void) {
    
    turn_off_led();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(device_identity_init("cam"));
    ESP_LOGI(TAG, "Device ID: %s", device_identity_get());

    ESP_ERROR_CHECK(wifi_manager_init());
    
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    while (wifi_manager_wait_connected(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGI(TAG, "Still waiting for WiFi...");
    }
    ESP_LOGI(TAG, "WiFi Connected!");

    ESP_ERROR_CHECK(cam_record_init());
    ESP_ERROR_CHECK(audio_record_init());

    ESP_ERROR_CHECK(ws_client_start(device_identity_get()));

    ESP_LOGI(TAG, "Waiting for WebSocket connection...");
    while (!ws_client_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "WebSocket Connected!");

    cam_stream_start();
    audio_stream_start();

    ESP_LOGI(TAG, "App Main finished. Tasks are running.");
}
