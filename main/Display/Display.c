#include "Display.h"
#include "lvgl.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"




/*static const char *TAG = "DISPLAY";*/

static spi_device_handle_t tft;

/* ---------------------------------------------------------------------- */
/* Low-level ILI9341 driver (unchanged from your original code)           */
/* ---------------------------------------------------------------------- */

static void ili9341_cmd(uint8_t cmd)
{
    gpio_set_level(TFT_DC, 0);

    spi_transaction_t trans = {
        .length = 8,
        .tx_buffer = &cmd
    };

    spi_device_transmit(tft, &trans);
}

static void ili9341_data(const uint8_t *data, size_t len)
{
    gpio_set_level(TFT_DC, 1);

    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = data
    };

    spi_device_transmit(tft, &trans);
}

static void ili9341_reset(void)
{
    gpio_set_level(TFT_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_set_level(TFT_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void ili9341_set_window(uint16_t x0, uint16_t y0,
                               uint16_t x1, uint16_t y1)
{
    uint8_t data[4];

    // Column address (0x2A)
    data[0] = x0 >> 8;
    data[1] = x0 & 0xFF;
    data[2] = x1 >> 8;
    data[3] = x1 & 0xFF;
    ili9341_cmd(0x2A);
    ili9341_data(data, 4);

    // Row address (0x2B) - direct, no manual flipping
    data[0] = y0 >> 8;
    data[1] = y0 & 0xFF;
    data[2] = y1 >> 8;
    data[3] = y1 & 0xFF;
    ili9341_cmd(0x2B);
    ili9341_data(data, 4);

    // Memory write (0x2C)
    ili9341_cmd(0x2C);
}

void ili9341_init(void)
{
    /* Configure control pins */
    gpio_config_t io_conf = {
        .pin_bit_mask =
            (1ULL << TFT_DC) |
            (1ULL << TFT_RESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    gpio_set_level(TFT_DC, 1);
    gpio_set_level(TFT_RESET, 1);

    /* Configure SPI bus */
    spi_bus_config_t buscfg = {
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = TFT_MISO,
        .sclk_io_num = TFT_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 240 * 320 * 2
    };

    ESP_ERROR_CHECK(
        spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO)
    );

    /* Configure TFT as an SPI device */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = TFT_CS,
        .queue_size = 1
    };

    ESP_ERROR_CHECK(
        spi_bus_add_device(SPI3_HOST, &devcfg, &tft)
    );

    ili9341_reset();

    ili9341_cmd(0x01);   /* Software reset */
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t pixel_format = 0x55;
    ili9341_cmd(0x3A);
    ili9341_data(&pixel_format, 1);

    uint8_t madctl = 0x00; /* 0x00 = RGB color order (clears BGR bit 3, fixing yellow tint to true cyan/blue) */
    ili9341_cmd(0x36);
    ili9341_data(&madctl, 1);

    ili9341_cmd(0x11);   /* Sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));

    ili9341_cmd(0x29);   /* Display ON */
    vTaskDelay(pdMS_TO_TICKS(20));
}



#define LVGL_BUF_ROWS 40
static lv_color_t buf1[240 * LVGL_BUF_ROWS];
static bool s_display_sleeping = false;

void ili9341_clear_black(void)
{
    ili9341_set_window(0, 0, TFT_WIDTH - 1, TFT_HEIGHT - 1);
    gpio_set_level(TFT_DC, 1);

    /* Stream black scanlines (RGB565 0x0000 = solid jet black) */
    static const uint8_t black_line[TFT_WIDTH * 2] = {0};
    for (int y = 0; y < TFT_HEIGHT; y++) {
        spi_transaction_t trans = {
            .length = sizeof(black_line) * 8,
            .tx_buffer = black_line
        };
        spi_device_transmit(tft, &trans);
    }
}

void ili9341_sleep_enter(void)
{
    if (s_display_sleeping) return;
    s_display_sleeping = true;
    ili9341_clear_black();
}

void ili9341_sleep_exit(void)
{
    if (!s_display_sleeping) return;
    s_display_sleeping = false;
    lv_obj_invalidate(lv_screen_active());
}

bool display_is_sleeping(void)
{
    return s_display_sleeping;
}

void display_set_sleeping(bool sleep)
{
    if (sleep) {
        ili9341_sleep_enter();
    } else {
        ili9341_sleep_exit();
    }
}

static void lvgl_flush_cb(
    lv_display_t *disp,
    const lv_area_t *area,
    uint8_t *px_map)
{
    if (s_display_sleeping) {
        lv_display_flush_ready(disp);
        return;
    }

    uint16_t w = area->x2 - area->x1 + 1;
    uint16_t h = area->y2 - area->y1 + 1;

/*ESP_LOGI(TAG,
         "FLUSH: x=%" PRId32 "..%" PRId32
         " y=%" PRId32 "..%" PRId32
         " w=%d h=%d",
         area->x1,
         area->x2,
         area->y1,
         area->y2,
         w,
         h);*/

    ili9341_set_window(
        area->x1,
        area->y1,
        area->x2,
        area->y2
    );

    gpio_set_level(TFT_DC, 1);

    spi_transaction_t trans = {
        .length = w * h * 16,
        .tx_buffer = px_map
    };

    ESP_ERROR_CHECK(spi_device_transmit(tft, &trans));

    lv_display_flush_ready(disp);
}

void lvgl_init(void)
{
    lv_init();

    lv_display_t *disp = lv_display_create(240, 320);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
}

void lvgl_tick_task(void *arg)
{
    while (1) {
        lv_tick_inc(10);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void lvgl_handler_task(void *arg)
{
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
