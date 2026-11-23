#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t   battery_init(void);
uint8_t     battery_get_percentage(void);
uint32_t    battery_get_voltage_mv(void);
const char *battery_get_symbol(void);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_H */
