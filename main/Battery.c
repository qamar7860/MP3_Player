#include "Battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "BATTERY";

#define BATTERY_ADC_UNIT     ADC_UNIT_1
#define BATTERY_ADC_CHANNEL  ADC_CHANNEL_6 /* GPIO 34 */

static adc_oneshot_unit_handle_t adc_handle = NULL;
static bool s_initialized = false;

esp_err_t battery_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ret = adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC config channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Battery monitor initialized on GPIO 34");
    return ESP_OK;
}

uint32_t battery_get_voltage_mv(void)
{
    if (!s_initialized || !adc_handle) {
        return 4200; /* Default 4.2V if uninitialized */
    }

    int raw = 0;
    int samples = 4;
    int sum = 0;
    for (int i = 0; i < samples; i++) {
        if (adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
        }
    }
    raw = sum / samples;

    /* 12-bit ADC: 0..4095 over approx 3300 mV (with 12dB attenuation) */
    /* If connected via 2:1 voltage divider (100k + 100k): battery_mv = adc_mv * 2 */
    uint32_t adc_mv = (uint32_t)((raw * 3300) / 4095);
    uint32_t bat_mv = adc_mv * 2;

    /* If pin is floating or 0 (running on USB with no battery sense resistor), fallback to 4.2V */
    if (bat_mv < 2500) {
        return 4200;
    }
    return bat_mv;
}

uint8_t battery_get_percentage(void)
{
    uint32_t mv = battery_get_voltage_mv();
    if (mv >= 4180) return 100;
    if (mv <= 3300) return 0;

    /* Linear interpolation between 3.3V (0%) and 4.2V (100%) */
    uint32_t pct = ((mv - 3300) * 100) / (4200 - 3300);
    return (uint8_t)(pct > 100 ? 100 : pct);
}

const char *battery_get_symbol(void)
{
    uint8_t pct = battery_get_percentage();
    if (pct >= 85) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 60) return LV_SYMBOL_BATTERY_3;
    if (pct >= 35) return LV_SYMBOL_BATTERY_2;
    if (pct >= 15) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}
