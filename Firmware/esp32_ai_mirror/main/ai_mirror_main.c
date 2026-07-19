/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * LVGL version: LCD display via LVGL + esp_lvgl_port.
 * Audio: I2S + ES8311 codec (music playback or echo mode).
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_system.h"
#include "esp_check.h"
#include "es8311.h"
#include "ai_mirror_config.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_lcd_gc9a01.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "board_buttons.h"
#include "board_rgb.h"
#include "board_wifi_prov.h"

/* Forward declaration */
extern void ai_mirror_ui_show_face_demo(lv_disp_t *disp);
extern void ai_mirror_ui_show_prov_qr(lv_disp_t *disp, const char *payload, const char *service_name, const char *transport);
extern void ai_mirror_ui_show_message(lv_disp_t *disp, const char *title, const char *line1, const char *line2, uint32_t bg_color, uint32_t accent_color);
extern void ai_mirror_ui_show_prov_select(lv_disp_t *disp, const char *selected_transport);
extern void ai_mirror_ui_show_internet_warning(lv_disp_t *disp, bool retry_selected);

/* ---- LCD pin definitions (unchanged) ---- */
#define LCD_HOST  SPI2_HOST

#define AI_MIRROR_LCD_PIXEL_CLOCK_HZ     (40 * 1000 * 1000)
#define AI_MIRROR_LCD_BK_LIGHT_ON_LEVEL  1
#define AI_MIRROR_LCD_BK_LIGHT_OFF_LEVEL !AI_MIRROR_LCD_BK_LIGHT_ON_LEVEL
#define AI_MIRROR_PIN_NUM_SCLK           18
#define AI_MIRROR_PIN_NUM_MOSI           19
#define AI_MIRROR_PIN_NUM_MISO           21
#define AI_MIRROR_PIN_NUM_LCD_DC         5
#define AI_MIRROR_PIN_NUM_LCD_RST        3
#define AI_MIRROR_PIN_NUM_LCD_CS         4
#define AI_MIRROR_PIN_NUM_BK_LIGHT       2
#define AI_MIRROR_PIN_NUM_TOUCH_CS       15

#define AI_MIRROR_LCD_H_RES              240
#define AI_MIRROR_LCD_V_RES              240
#define AI_MIRROR_LCD_CMD_BITS           8
#define AI_MIRROR_LCD_PARAM_BITS         8

/* ================================================================
 *  Audio section (unchanged from original)
 * ================================================================ */
static const char *TAG = "ai_mirror";
static const char err_reason[][30] = {"input param is invalid",
                                      "operation timeout"
                                     };
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;

static void app_button_event_cb(board_button_id_t button, board_button_event_t event, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "app button event: button=%d, event=%s",
             button + 1,
             event == BOARD_BUTTON_EVENT_PRESSED ? "pressed" : "released");
}

#if CONFIG_AI_MIRROR_AUDIO_MODE_MUSIC
extern const uint8_t music_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t music_pcm_end[]   asm("_binary_canon_pcm_end");
#endif

static esp_err_t es8311_codec_init(void)
{
#if !defined(CONFIG_AI_MIRROR_BSP)
    const i2c_config_t es_i2c_cfg = {
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .mode = I2C_MODE_MASTER,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM, &es_i2c_cfg), TAG, "config i2c failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM, I2C_MODE_MASTER,  0, 0, 0), TAG, "install i2c driver failed");
#else
    ESP_ERROR_CHECK(bsp_i2c_init());
#endif

    es8311_handle_t es_handle = es8311_create(I2C_NUM, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");
    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = AI_MIRROR_MCLK_FREQ_HZ,
        .sample_frequency = AI_MIRROR_SAMPLE_RATE
    };

    ESP_RETURN_ON_ERROR(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "es8311 init failed");
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, AI_MIRROR_SAMPLE_RATE * AI_MIRROR_MCLK_MULTIPLE, AI_MIRROR_SAMPLE_RATE), TAG, "set es8311 sample frequency failed");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, AI_MIRROR_VOICE_VOLUME, NULL), TAG, "set es8311 volume failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");
#if CONFIG_AI_MIRROR_AUDIO_MODE_ECHO
    ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(es_handle, AI_MIRROR_MIC_GAIN), TAG, "set es8311 microphone gain failed");
#endif
    return ESP_OK;
}

static esp_err_t i2s_driver_init(void)
{
#if !defined(CONFIG_AI_MIRROR_BSP)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AI_MIRROR_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = AI_MIRROR_MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
#else
    ESP_LOGI(TAG, "Using BSP for HW configuration");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AI_MIRROR_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };
    std_cfg.clk_cfg.mclk_multiple = AI_MIRROR_MCLK_MULTIPLE;
    ESP_ERROR_CHECK(bsp_audio_init(&std_cfg, &tx_handle, &rx_handle));
    ESP_ERROR_CHECK(bsp_audio_poweramp_enable(true));
#endif
    return ESP_OK;
}

#if CONFIG_AI_MIRROR_AUDIO_MODE_MUSIC
static void i2s_music(void *args)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_write = 0;
    uint8_t *data_ptr = (uint8_t *)music_pcm_start;

    ESP_ERROR_CHECK(i2s_channel_disable(tx_handle));
    ESP_ERROR_CHECK(i2s_channel_preload_data(tx_handle, data_ptr, music_pcm_end - data_ptr, &bytes_write));
    data_ptr += bytes_write;

    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    while (1) {
        ret = i2s_channel_write(tx_handle, data_ptr, music_pcm_end - data_ptr, &bytes_write, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[music] i2s write failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
            abort();
        }
        if (bytes_write > 0) {
            ESP_LOGI(TAG, "[music] i2s music played, %d bytes are written.", bytes_write);
        } else {
            ESP_LOGE(TAG, "[music] i2s music play failed.");
            abort();
        }
        data_ptr = (uint8_t *)music_pcm_start;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

#else
static void i2s_echo(void *args)
{
    int *mic_data = malloc(AI_MIRROR_RECV_BUF_SIZE);
    if (!mic_data) {
        ESP_LOGE(TAG, "[echo] No memory for read data buffer");
        abort();
    }
    esp_err_t ret = ESP_OK;
    size_t bytes_read = 0;
    size_t bytes_write = 0;
    ESP_LOGI(TAG, "[echo] Echo start");

    while (1) {
        memset(mic_data, 0, AI_MIRROR_RECV_BUF_SIZE);
        ret = i2s_channel_read(rx_handle, mic_data, AI_MIRROR_RECV_BUF_SIZE, &bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[echo] i2s read failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
            abort();
        }
        ret = i2s_channel_write(tx_handle, mic_data, AI_MIRROR_RECV_BUF_SIZE, &bytes_write, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[echo] i2s write failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
            abort();
        }
        if (bytes_read != bytes_write) {
            ESP_LOGW(TAG, "[echo] %d bytes read but only %d bytes are written", bytes_read, bytes_write);
        }
    }
    vTaskDelete(NULL);
}
#endif


/* ================================================================
 *  UI / input / network orchestration
 * ================================================================ */
#define AI_MIRROR_BOOT_PROV_HOLD_MS        2000
#define AI_MIRROR_BOOT_DECISION_WINDOW_MS  3500
#define AI_MIRROR_WIFI_CONNECT_TIMEOUT_MS  20000
#define AI_MIRROR_INTERNET_TIMEOUT_MS      6000
#define AI_MIRROR_LVGL_BUFFER_LINES        40

/* Product-default behavior: missing saved Wi-Fi forces provisioning.
 * If you really want saved Wi-Fi to also force provisioning at every boot, flip this to 1. */
#define AI_MIRROR_FORCE_PROV_WHEN_WIFI_SAVED 0

static void ui_show_message(lv_disp_t *disp, const char *title, const char *line1,
                            const char *line2, uint32_t bg, uint32_t accent)
{
    lvgl_port_lock(0);
    ai_mirror_ui_show_message(disp, title, line1, line2, bg, accent);
    lvgl_port_unlock();
}

static void wait_button_release(board_button_id_t button)
{
    while (board_button_is_pressed(button)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool wait_button_long_press(board_button_id_t button, uint32_t hold_ms, uint32_t window_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t pressed_since = 0;
    const TickType_t hold_ticks = pdMS_TO_TICKS(hold_ms);
    const TickType_t window_ticks = pdMS_TO_TICKS(window_ms);

    while ((xTaskGetTickCount() - start) < window_ticks) {
        if (board_button_is_pressed(button)) {
            if (pressed_since == 0) {
                pressed_since = xTaskGetTickCount();
            }
            if ((xTaskGetTickCount() - pressed_since) >= hold_ticks) {
                wait_button_release(button);
                return true;
            }
        } else {
            pressed_since = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return false;
}

static board_wifi_prov_transport_t select_provisioning_transport(lv_disp_t *disp)
{
    board_wifi_prov_transport_t selected = BOARD_WIFI_PROV_TRANSPORT_SOFTAP;

    while (1) {
        lvgl_port_lock(0);
        ai_mirror_ui_show_prov_select(disp, board_wifi_prov_transport_name(selected));
        lvgl_port_unlock();

        while (1) {
            if (board_button_is_pressed(BOARD_BUTTON_ID_2)) {
                wait_button_release(BOARD_BUTTON_ID_2);
                selected = selected == BOARD_WIFI_PROV_TRANSPORT_SOFTAP ?
                           BOARD_WIFI_PROV_TRANSPORT_BLE : BOARD_WIFI_PROV_TRANSPORT_SOFTAP;
                break;
            }
            if (board_button_is_pressed(BOARD_BUTTON_ID_1)) {
                wait_button_release(BOARD_BUTTON_ID_1);
                return selected;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}

static bool ask_retry_provisioning(lv_disp_t *disp)
{
    bool retry = true;

    while (1) {
        lvgl_port_lock(0);
        ai_mirror_ui_show_internet_warning(disp, retry);
        lvgl_port_unlock();

        while (1) {
            if (board_button_is_pressed(BOARD_BUTTON_ID_2)) {
                wait_button_release(BOARD_BUTTON_ID_2);
                retry = !retry;
                break;
            }
            if (board_button_is_pressed(BOARD_BUTTON_ID_1)) {
                wait_button_release(BOARD_BUTTON_ID_1);
                return retry;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}

static bool internet_check_url(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = AI_MIRROR_INTERNET_TIMEOUT_MS,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "internet check: url=%s err=%s status=%d", url, esp_err_to_name(err), status);
    return err == ESP_OK && status >= 200 && status < 400;
}

static bool ai_mirror_check_internet(void)
{
    static const char *urls[] = {
        "http://connectivitycheck.gstatic.com/generate_204",
        "http://www.espressif.com",
    };

    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        if (internet_check_url(urls[i])) {
            return true;
        }
    }
    return false;
}

static bool run_connectivity_check_or_prompt(lv_disp_t *disp)
{
    ui_show_message(disp, "Checking internet", "Please wait", "", 0xF8FBFF, 0x6EA8FF);
    if (ai_mirror_check_internet()) {
        ui_show_message(disp, "Internet OK", "Starting AI Mirror", "", 0xECFDF3, 0x2FD681);
        vTaskDelay(pdMS_TO_TICKS(700));
        return true;
    }

    ESP_LOGW(TAG, "Wi-Fi connected but internet check failed");
    return !ask_retry_provisioning(disp);
}

static bool run_provisioning_flow(lv_disp_t *disp)
{
    while (1) {
        board_wifi_prov_transport_t transport = select_provisioning_transport(disp);
        ui_show_message(disp, "Starting setup", board_wifi_prov_transport_name(transport), "Please wait", 0xF8FBFF, 0x6EA8FF);

        esp_err_t prov_ret = board_wifi_prov_start(transport, true);
        if (prov_ret != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning start failed: transport=%s err=%s",
                     board_wifi_prov_transport_name(transport), esp_err_to_name(prov_ret));
            ui_show_message(disp,
                            "Setup failed",
                            board_wifi_prov_transport_name(transport),
                            "BTN2/BTN1 return",
                            0xFFF1F2,
                            0xF43F5E);
            wait_button_release(BOARD_BUTTON_ID_1);
            wait_button_release(BOARD_BUTTON_ID_2);
            while (1) {
                if (board_button_is_pressed(BOARD_BUTTON_ID_2)) {
                    wait_button_release(BOARD_BUTTON_ID_2);
                    break;
                }
                if (board_button_is_pressed(BOARD_BUTTON_ID_1)) {
                    wait_button_release(BOARD_BUTTON_ID_1);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            continue;
        }

        lvgl_port_lock(0);
        ai_mirror_ui_show_prov_qr(disp,
                                  board_wifi_prov_get_qr_payload(),
                                  board_wifi_prov_get_service_name(),
                                  board_wifi_prov_transport_name(board_wifi_prov_get_transport()));
        lvgl_port_unlock();

        ESP_LOGI(TAG, "Waiting for provisioning Wi-Fi connection, BTN2 returns to mode selection...");
        bool go_back_to_select = false;
        while (!board_wifi_prov_wait_connected(100)) {
            if (board_button_is_pressed(BOARD_BUTTON_ID_2)) {
                wait_button_release(BOARD_BUTTON_ID_2);
                go_back_to_select = true;
                ui_show_message(disp, "Back to setup", "Stopping current mode", "Please wait", 0xF8FBFF, 0x6EA8FF);
                board_wifi_prov_stop(5000);
                break;
            }
        }

        if (go_back_to_select) {
            continue;
        }

        if (run_connectivity_check_or_prompt(disp)) {
            return true;
        }
    }
}

static bool ai_mirror_audio_init(void)
{
    bool audio_ok = true;
    if (i2s_driver_init() != ESP_OK) {
        ESP_LOGE(TAG, "i2s driver init failed, skip audio");
        audio_ok = false;
    } else {
        ESP_LOGI(TAG, "i2s driver init success");
    }
    if (audio_ok) {
        if (es8311_codec_init() != ESP_OK) {
            ESP_LOGE(TAG, "es8311 codec init failed, skip audio");
            audio_ok = false;
        } else {
            ESP_LOGI(TAG, "es8311 codec init success");
        }
    }
    return audio_ok;
}

static void ai_mirror_audio_start(bool audio_ok)
{
#if CONFIG_AI_MIRROR_AUDIO_MODE_MUSIC
    if (audio_ok) {
        ESP_LOGI(TAG, "Start music playback");
        xTaskCreate(i2s_music, "i2s_music", 2*4096, NULL, 5, NULL);
    } else {
        ESP_LOGW(TAG, "Audio disabled, music task not started");
    }
#else
    if (audio_ok) {
        ESP_LOGI(TAG, "Start echo mode");
        xTaskCreate(i2s_echo, "i2s_echo", 8192, NULL, 5, NULL);
    } else {
        ESP_LOGW(TAG, "Audio disabled, echo task not started");
    }
#endif
}

static lv_disp_t *ai_mirror_display_init(void)
{
    ESP_LOGI(TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << AI_MIRROR_PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = AI_MIRROR_PIN_NUM_SCLK,
        .mosi_io_num = AI_MIRROR_PIN_NUM_MOSI,
        .miso_io_num = AI_MIRROR_PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = AI_MIRROR_LCD_H_RES * AI_MIRROR_LVGL_BUFFER_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = AI_MIRROR_PIN_NUM_LCD_DC,
        .cs_gpio_num = AI_MIRROR_PIN_NUM_LCD_CS,
        .pclk_hz = AI_MIRROR_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = AI_MIRROR_LCD_CMD_BITS,
        .lcd_param_bits = AI_MIRROR_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 20,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = AI_MIRROR_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_LOGI(TAG, "Install GC9A01 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Initialize LVGL port");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = AI_MIRROR_LCD_H_RES * AI_MIRROR_LVGL_BUFFER_LINES,
        .double_buffer = true,
        .hres = AI_MIRROR_LCD_H_RES,
        .vres = AI_MIRROR_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "LVGL display allocation failed");
        abort();
    }

    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(AI_MIRROR_PIN_NUM_BK_LIGHT, AI_MIRROR_LCD_BK_LIGHT_ON_LEVEL);
    return disp;
}

/* ================================================================
 *  Main
 * ================================================================ */
void app_main(void)
{
    printf("ai_mirror start (LVGL display)\n");
    printf("-----------------------------\n");

    ESP_ERROR_CHECK(board_rgb_init());
    board_rgb_start_rainbow_demo();
    ESP_ERROR_CHECK(board_buttons_init(app_button_event_cb, NULL));

    lv_disp_t *disp = ai_mirror_display_init();
    ui_show_message(disp, "AI Mirror", "Starting", "Please wait", 0xF8FBFF, 0x6EA8FF);

    bool audio_ok = ai_mirror_audio_init();

    ESP_ERROR_CHECK(board_wifi_prov_prepare());
    bool has_saved_credentials = false;
    ESP_ERROR_CHECK(board_wifi_prov_has_saved_credentials(&has_saved_credentials));

    bool manual_provisioning = false;
    if (has_saved_credentials) {
        ui_show_message(disp, "AI Mirror", "Hold BTN1 2s", "to enter WiFi setup", 0xF8FBFF, 0x6EA8FF);
        manual_provisioning = wait_button_long_press(BOARD_BUTTON_ID_1,
                                                     AI_MIRROR_BOOT_PROV_HOLD_MS,
                                                     AI_MIRROR_BOOT_DECISION_WINDOW_MS);
    } else {
        ui_show_message(disp, "No WiFi saved", "Entering setup", "Please choose mode", 0xF8FBFF, 0x6EA8FF);
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    bool force_provisioning = !has_saved_credentials ||
                              (AI_MIRROR_FORCE_PROV_WHEN_WIFI_SAVED && has_saved_credentials);
    bool enter_provisioning = manual_provisioning || force_provisioning;

    ESP_LOGI(TAG, "boot decision: saved_wifi=%s manual_prov=%s enter_prov=%s",
             has_saved_credentials ? "yes" : "no",
             manual_provisioning ? "yes" : "no",
             enter_provisioning ? "yes" : "no");

    bool ready_for_ui = false;
    if (enter_provisioning) {
        ready_for_ui = run_provisioning_flow(disp);
    } else {
        ui_show_message(disp, "Connecting WiFi", "Using saved network", "Please wait", 0xF8FBFF, 0x6EA8FF);
        esp_err_t connect_ret = board_wifi_prov_connect_saved(AI_MIRROR_WIFI_CONNECT_TIMEOUT_MS);
        if (connect_ret == ESP_OK) {
            ready_for_ui = run_connectivity_check_or_prompt(disp);
            if (!ready_for_ui) {
                ready_for_ui = run_provisioning_flow(disp);
            }
        } else {
            ESP_LOGW(TAG, "saved Wi-Fi connection failed: %s", esp_err_to_name(connect_ret));
            ui_show_message(disp, "WiFi failed", "Saved network failed", "BTN1 setup / BTN2 skip", 0xFFF7ED, 0xF97316);
            vTaskDelay(pdMS_TO_TICKS(600));
            if (ask_retry_provisioning(disp)) {
                ready_for_ui = run_provisioning_flow(disp);
            } else {
                ready_for_ui = true;
            }
        }
    }

    if (ready_for_ui) {
        ESP_LOGI(TAG, "Start LVGL cute face animation");
        lvgl_port_lock(0);
        ai_mirror_ui_show_face_demo(disp);
        lvgl_port_unlock();
    }

    ai_mirror_audio_start(audio_ok);
}