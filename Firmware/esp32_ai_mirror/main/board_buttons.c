#include "board_buttons.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUTTON_SCAN_INTERVAL_MS 10
#define BUTTON_DEBOUNCE_MS      30

typedef struct {
    gpio_num_t gpio;
    bool stable_pressed;
    bool last_sample_pressed;
    uint32_t debounce_ticks;
} button_state_t;

static const char *TAG = "board_buttons";
static board_button_event_cb_t s_button_cb;
static void *s_button_user_ctx;
static button_state_t s_buttons[BOARD_BUTTON_ID_MAX] = {
    [BOARD_BUTTON_ID_1] = {.gpio = BOARD_BUTTON_1_GPIO},
    [BOARD_BUTTON_ID_2] = {.gpio = BOARD_BUTTON_2_GPIO},
};

static bool read_pressed(gpio_num_t gpio)
{
    return gpio_get_level(gpio) == 1;
}

static void button_scan_task(void *arg)
{
    const uint32_t debounce_ticks = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);

    while (1) {
        for (board_button_id_t i = 0; i < BOARD_BUTTON_ID_MAX; i++) {
            button_state_t *button = &s_buttons[i];
            bool sample_pressed = read_pressed(button->gpio);

            if (sample_pressed != button->last_sample_pressed) {
                button->last_sample_pressed = sample_pressed;
                button->debounce_ticks = xTaskGetTickCount();
            }

            if ((xTaskGetTickCount() - button->debounce_ticks) >= debounce_ticks &&
                sample_pressed != button->stable_pressed) {
                button->stable_pressed = sample_pressed;
                board_button_event_t event = sample_pressed ? BOARD_BUTTON_EVENT_PRESSED : BOARD_BUTTON_EVENT_RELEASED;

                ESP_LOGI(TAG, "button %d %s", i + 1, sample_pressed ? "pressed" : "released");
                if (s_button_cb) {
                    s_button_cb(i, event, s_button_user_ctx);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_INTERVAL_MS));
    }
}

esp_err_t board_buttons_init(board_button_event_cb_t callback, void *user_ctx)
{
    uint64_t pin_mask = (1ULL << BOARD_BUTTON_1_GPIO) | (1ULL << BOARD_BUTTON_2_GPIO);
    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    s_button_cb = callback;
    s_button_user_ctx = user_ctx;

    for (board_button_id_t i = 0; i < BOARD_BUTTON_ID_MAX; i++) {
        s_buttons[i].stable_pressed = read_pressed(s_buttons[i].gpio);
        s_buttons[i].last_sample_pressed = s_buttons[i].stable_pressed;
        s_buttons[i].debounce_ticks = xTaskGetTickCount();
    }

    xTaskCreate(button_scan_task, "button_scan", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "buttons initialized: BTN1=GPIO%d, BTN2=GPIO%d, active high",
             BOARD_BUTTON_1_GPIO, BOARD_BUTTON_2_GPIO);
    return ESP_OK;
}

bool board_button_is_pressed(board_button_id_t button)
{
    if (button >= BOARD_BUTTON_ID_MAX) {
        return false;
    }
    return s_buttons[button].stable_pressed;
}
