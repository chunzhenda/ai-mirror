#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_RGB_GPIO      48
#define BOARD_RGB_LED_COUNT 1

esp_err_t board_rgb_init(void);
esp_err_t board_rgb_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
void board_rgb_start_rainbow_demo(void);

#ifdef __cplusplus
}
#endif
