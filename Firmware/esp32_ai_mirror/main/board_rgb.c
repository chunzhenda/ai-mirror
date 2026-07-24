#include "board_rgb.h"

#include <stdlib.h>
#include <string.h>
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WS2812_RESOLUTION_HZ 10000000
#define WS2812_RESET_US      280

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

static const char *TAG = "board_rgb";
static rmt_channel_handle_t s_led_chan;
static rmt_encoder_handle_t s_led_encoder;
static TaskHandle_t s_rainbow_task;
static uint8_t s_led_pixels[BOARD_RGB_LED_COUNT * 3];

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *primary_data, size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws2812_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (ws2812_encoder->state) {
    case 0:
        encoded_symbols += ws2812_encoder->bytes_encoder->encode(ws2812_encoder->bytes_encoder, channel,
                                                                  primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws2812_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            break;
        }
        /* fall through */
    case 1:
        encoded_symbols += ws2812_encoder->copy_encoder->encode(ws2812_encoder->copy_encoder, channel,
                                                                 &ws2812_encoder->reset_code,
                                                                 sizeof(ws2812_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws2812_encoder->state = 0;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
        break;
    default:
        break;
    }

    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t ws2812_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws2812_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(ws2812_encoder->bytes_encoder);
    rmt_del_encoder(ws2812_encoder->copy_encoder);
    free(ws2812_encoder);
    return ESP_OK;
}

static esp_err_t ws2812_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws2812_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(ws2812_encoder->bytes_encoder);
    rmt_encoder_reset(ws2812_encoder->copy_encoder);
    ws2812_encoder->state = 0;
    return ESP_OK;
}

static esp_err_t ws2812_new_encoder(rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    ws2812_encoder_t *ws2812_encoder = calloc(1, sizeof(ws2812_encoder_t));
    ESP_RETURN_ON_FALSE(ws2812_encoder, ESP_ERR_NO_MEM, TAG, "no mem for ws2812 encoder");

    ws2812_encoder->base.encode = ws2812_encode;
    ws2812_encoder->base.del = ws2812_del;
    ws2812_encoder->base.reset = ws2812_reset;

    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 0.3 * WS2812_RESOLUTION_HZ / 1000000,
            .level1 = 0,
            .duration1 = 0.9 * WS2812_RESOLUTION_HZ / 1000000,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 0.9 * WS2812_RESOLUTION_HZ / 1000000,
            .level1 = 0,
            .duration1 = 0.3 * WS2812_RESOLUTION_HZ / 1000000,
        },
        .flags.msb_first = 1,
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_encoder_config, &ws2812_encoder->bytes_encoder),
                      err, TAG, "create bytes encoder failed");

    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_config, &ws2812_encoder->copy_encoder),
                      err, TAG, "create copy encoder failed");

    ws2812_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = (uint32_t)WS2812_RESET_US * WS2812_RESOLUTION_HZ / 1000000,
        .level1 = 0,
        .duration1 = (uint32_t)WS2812_RESET_US * WS2812_RESOLUTION_HZ / 1000000,
    };
    *ret_encoder = &ws2812_encoder->base;
    return ESP_OK;

err:
    if (ws2812_encoder) {
        if (ws2812_encoder->bytes_encoder) {
            rmt_del_encoder(ws2812_encoder->bytes_encoder);
        }
        if (ws2812_encoder->copy_encoder) {
            rmt_del_encoder(ws2812_encoder->copy_encoder);
        }
        free(ws2812_encoder);
    }
    return ret;
}

static void hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value,
                       uint8_t *red, uint8_t *green, uint8_t *blue)
{
    uint8_t region = hue / 60;
    uint16_t remainder = (hue - (region * 60)) * 255 / 60;
    uint8_t p = (value * (255 - saturation)) / 255;
    uint8_t q = (value * (255 - ((saturation * remainder) / 255))) / 255;
    uint8_t t = (value * (255 - ((saturation * (255 - remainder)) / 255))) / 255;

    switch (region) {
    case 0:
        *red = value;
        *green = t;
        *blue = p;
        break;
    case 1:
        *red = q;
        *green = value;
        *blue = p;
        break;
    case 2:
        *red = p;
        *green = value;
        *blue = t;
        break;
    case 3:
        *red = p;
        *green = q;
        *blue = value;
        break;
    case 4:
        *red = t;
        *green = p;
        *blue = value;
        break;
    default:
        *red = value;
        *green = p;
        *blue = q;
        break;
    }
}

esp_err_t board_rgb_init(void)
{
    if (s_led_chan) {
        return ESP_OK;
    }

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = BOARD_RGB_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_chan_config, &s_led_chan), TAG, "create RMT TX channel failed");
    ESP_RETURN_ON_ERROR(ws2812_new_encoder(&s_led_encoder), TAG, "create WS2812 encoder failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_chan), TAG, "enable RMT channel failed");
    return board_rgb_set_rgb(0, 0, 0);
}

esp_err_t board_rgb_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(s_led_chan && s_led_encoder, ESP_ERR_INVALID_STATE, TAG, "RGB is not initialized");

    s_led_pixels[0] = green;
    s_led_pixels[1] = red;
    s_led_pixels[2] = blue;

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    ESP_RETURN_ON_ERROR(rmt_transmit(s_led_chan, s_led_encoder, s_led_pixels,
                                     sizeof(s_led_pixels), &tx_config),
                        TAG, "transmit RGB frame failed");
    return rmt_tx_wait_all_done(s_led_chan, pdMS_TO_TICKS(100));
}

static void rgb_rainbow_task(void *arg)
{
    uint16_t hue = 0;

    while (1) {
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        hsv_to_rgb(hue, 255, 32, &red, &green, &blue);
        ESP_ERROR_CHECK_WITHOUT_ABORT(board_rgb_set_rgb(red, green, blue));

        hue = (hue + 2) % 360;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void board_rgb_start_rainbow_demo(void)
{
    if (s_rainbow_task) {
        return;
    }

    BaseType_t ok = xTaskCreate(rgb_rainbow_task, "rgb_rainbow", 4096, NULL, 2, &s_rainbow_task);
    if (ok != pdPASS) {
        s_rainbow_task = NULL;
        ESP_LOGE(TAG, "failed to create RGB rainbow task");
    }
}
