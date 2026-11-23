#include "Temperature.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "soc/sens_struct.h"
#include "soc/sens_reg.h"

static const char *TAG = "TEMP";
static float s_last_valid_temp = 42.0f;

esp_err_t temperature_init(void)
{
    ESP_LOGI(TAG, "Initializing ESP32 internal chip temperature sensor");
    SENS.sar_tctrl.tsens_clk_div = 6;
    SENS.sar_tctrl.tsens_power_up_force = 1;
    SENS.sar_tctrl.tsens_power_up = 1;
    SENS.sar_tctrl.tsens_dump_out = 0;
    return ESP_OK;
}

float temperature_read_celsius(void)
{
    SENS.sar_tctrl.tsens_dump_out = 0;
    esp_rom_delay_us(80);
    SENS.sar_tctrl.tsens_dump_out = 1;
    esp_rom_delay_us(80);

    uint32_t raw = SENS.sar_slave_addr3.tsens_out;
    if (raw == 0 || raw == 255) {
        return s_last_valid_temp;
    }

    /* Convert raw TRM register output to Celsius */
    float celsius = ((float)raw - 32.0f) / 1.8f;
    s_last_valid_temp = celsius;
    return celsius;
}
