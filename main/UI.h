#ifndef UI_H
#define UI_H

#include "lvgl.h"
#include <stdbool.h>

/* Minimized 3-Button Architecture:
 * Headphones handle:
 *   Right: 1x Play/Pause, 2x Next track, Hold Volume Up
 *   Left:  1x Play/Pause, 2x Prev track, Hold Volume Down
 *
 * Device Physical Buttons (3 Buttons Total):
 *   Button 1: PIN_POWER_SLEEP (GPIO 32)
 *             - Single click: Power OFF (Deep sleep, wake on GPIO 32)
 *             - Hold (>600ms): Sleep Mode / Battery Saver (Screen off, music keeps playing)
 *   Button 2: PIN_NAV (GPIO 33)
 *             - Single click: Screen Navigation (Home -> Lib -> BT -> Settings) / Next item
 *             - Hold (>500ms): Back to Home screen
 *   Button 3: PIN_ACTION (GPIO 25)
 *             - Single click: Play/Pause in Home / Select in Library / Connect in BT
 *             - Hold (>500ms): Toggle Shuffle Mode
 */
#define PIN_POWER_SLEEP     32
#define PIN_NAV             33
#define PIN_ACTION          25

/* Aliases */
#define PIN_POWER           PIN_POWER_SLEEP
#define PIN_BATTERY_SAVER   PIN_POWER_SLEEP
#define PIN_PLAY            PIN_ACTION

void ui_init(void);
void buttons_gpio_init(void);

void enter_battery_saver_mode(void);
void exit_battery_saver_mode(void);
bool is_battery_saver_mode(void);
void power_off_device(void);

#endif
