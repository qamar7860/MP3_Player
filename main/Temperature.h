#ifndef TEMPERATURE_H
#define TEMPERATURE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize internal ESP32 chip temperature monitoring
 */
esp_err_t temperature_init(void);

/**
 * @brief Read internal ESP32 chip temperature in degrees Celsius
 * @return Temperature in Celsius (e.g. 42.5 C)
 */
float temperature_read_celsius(void);

#ifdef __cplusplus
}
#endif

#endif // TEMPERATURE_H
