#include <stdio.h>
#include <stdint.h>
#include "UI.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"        // ESP_LOGI
#include "esp_system.h"     // esp_get_free_heap_size()
#include "esp_heap_caps.h"  // heap_caps_get_largest_free_block(), MALLOC_CAP_INTERNAL
#include "Display.h"
#include "lvgl.h"
#include "SD_Card.h"
#include "Bluetooth.h"
#include "Player.h"
#include "Battery.h"
#include "Temperature.h"
#include "esp_err.h"
#include "nvs_flash.h"

//void mp3_test_start(void); 
void sd_bench(void);

void app_main(void)
{
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    ili9341_init();
    battery_init();
    temperature_init();

    esp_err_t ret = sdcard_init();

    if (ret != ESP_OK)
    {
        ESP_LOGE("SD", "SD initialization failed: %s",
                 esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI("SD", "SD card initialized successfully!");
        scan_mp3_files();
    }
    sd_bench();
    player_init();
    lvgl_init();
    ui_init();
    bt_init();
			 
			
    /* Dual-core architecture:
     * Core 0: Dedicated to Bluetooth Classic Controller & Bluedroid (A2DP Source, AVRCP TG/CT)
     * Core 1: Dedicated to Audio MP3 decoding (player_task) and UI/LVGL engine
     */
    xTaskCreatePinnedToCore(
        lvgl_tick_task,
        "lvgl_tick",
        2048,
        NULL,
        5,
        NULL,
        1
    );

    /* Pinned to Core 1 at Priority 3 (lower than player_task at 6) to avoid audio starvation */
    xTaskCreatePinnedToCore(
        lvgl_handler_task,
        "lvgl_handler",
        8192,
        NULL,
        3,
        NULL,
        1
    );

	ESP_LOGI("MEM", "free heap: %u, largest block: %u",
	         (unsigned)esp_get_free_heap_size(),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
			 
			 
}
