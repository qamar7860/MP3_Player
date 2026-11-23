#ifndef DISPLAY_H
#define DISPLAY_H


/*****************************************************************************************************************************
************************************************Display Pins******************************************************************
*******************************************************************************************************************************/
#define TFT_MISO 19
//lcd
#define TFT_SCLK 18
#define TFT_MOSI 23

#define TFT_DC    2
#define TFT_RESET 4
#define TFT_CS    5

#define TFT_WIDTH  240
#define TFT_HEIGHT 320



void ili9341_init(void);       
void buttons_gpio_init(void);
void lvgl_init(void);
void lvgl_tick_task(void *arg);
void lvgl_handler_task(void *arg);

void ili9341_clear_black(void);
void ili9341_sleep_enter(void);
void ili9341_sleep_exit(void);
bool display_is_sleeping(void);
void display_set_sleeping(bool sleep);

#endif
