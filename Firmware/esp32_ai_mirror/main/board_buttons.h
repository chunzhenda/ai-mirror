#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_BUTTON_1_GPIO GPIO_NUM_13
#define BOARD_BUTTON_2_GPIO GPIO_NUM_14

typedef enum {
    BOARD_BUTTON_ID_1 = 0,
    BOARD_BUTTON_ID_2,
    BOARD_BUTTON_ID_MAX,
} board_button_id_t;

typedef enum {
    BOARD_BUTTON_EVENT_PRESSED = 0,
    BOARD_BUTTON_EVENT_RELEASED,
} board_button_event_t;

typedef void (*board_button_event_cb_t)(board_button_id_t button,
                                        board_button_event_t event,
                                        void *user_ctx);

esp_err_t board_buttons_init(board_button_event_cb_t callback, void *user_ctx);
bool board_button_is_pressed(board_button_id_t button);

#ifdef __cplusplus
}
#endif
