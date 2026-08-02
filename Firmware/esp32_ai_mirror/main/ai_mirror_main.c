/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * LVGL version: LCD display via LVGL + esp_lvgl_port.
 * Audio: I2S + ES8311 codec (music playback or echo mode).
 */

#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "driver/i2s_std.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_aec.h"
#include "esp_vad.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "opus.h"
#include <math.h>
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
#include "ai_mirror_gateway.h"
#include "ai_mirror_audio_playback.h"

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
#if CONFIG_AI_MIRROR_AUDIO_MODE_MUSIC
static const char err_reason[][30] = {"input param is invalid", "operation timeout"};
#endif
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
    /* 麦克风增益设置偶发 I²C 写失败(瞬时 NACK),重试几次;仍失败则降级
     * 为警告 - 增益设不上仍可使用默认增益录音,不应让整个音频系统禁用
     * 而阻塞 AEC 演示与后续调试。 */
    esp_err_t mic_gain_ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        mic_gain_ret = es8311_microphone_gain_set(es_handle, AI_MIRROR_MIC_GAIN);
        if (mic_gain_ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "set es8311 microphone gain failed (attempt %d, 0x%x)", attempt + 1, mic_gain_ret);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (mic_gain_ret != ESP_OK) {
        ESP_LOGW(TAG, "microphone gain set failed after retries, continuing with default gain");
    }
#endif
    return ESP_OK;
}

static esp_err_t i2s_driver_init(void)
{
#if !defined(CONFIG_AI_MIRROR_BSP)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 480;
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
/* ----------------------------------------------------------------
 * AEC (Acoustic Echo Cancellation) demo mode.
 *
 * The speaker stays silent during recording (no far-end reference), so
 * AEC runs with a zero reference and just passes the cleaned mic signal
 * through. Press BTN1 once to start recording (no need to hold); a start
 * chime plays as feedback. VAD then watches the cleaned mic and ends the
 * recording when it detects trailing silence after speech. The recording is
 * opus-encoded, opus-decoded, then played back immediately. Pressing BTN1
 * during encode/decode/playback drops that recording and starts a new one.
 * ---------------------------------------------------------------- */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AEC_FRAME_MS            16
#define AEC_FRAME_SAMPLES       (AI_MIRROR_SAMPLE_RATE * AEC_FRAME_MS / 1000)
#define AEC_STEREO_SAMPLES      (AEC_FRAME_SAMPLES * 2)
#define AEC_FRAME_BYTES         (AEC_STEREO_SAMPLES * sizeof(int16_t))
#define AEC_REC_MAX_SECONDS     20
#define AEC_REC_BUF_SAMPLES     (AI_MIRROR_SAMPLE_RATE * AEC_REC_MAX_SECONDS)
#define AEC_SESSION_TIMEOUT_MS  30000
#define AEC_REPLY_TIMEOUT_MS    120000
#define AEC_SESSION_GUARD_MS    800
#define AEC_SESSION_PREROLL_MS  400
#define AEC_SESSION_PREROLL_SAMPLES \
    (AI_MIRROR_SAMPLE_RATE * AEC_SESSION_PREROLL_MS / 1000)
#define AEC_SESSION_VAD_RUN_FRAMES  3
#define AEC_SESSION_MIN_LEVEL       60
#define AEC_SESSION_NOISE_MULTIPLIER 1
#define AEC_SESSION_MAX_DYNAMIC_LEVEL 300
#define AEC_VAD_SILENCE_END_MS  1000    /* ms: trailing silence after speech -> "user stopped talking" */
#define AEC_VAD_TAIL_DROP_MS    900   /* ms of tail silence to drop before decode (< VAD 1s threshold) */
#define AEC_VAD_TAIL_DROP_FRAMES  (AEC_VAD_TAIL_DROP_MS / VAD_FRAME_MS)  /* 45 opus frames (~0.9s) */
#define AEC_CHIME_ATTACK_MS      6      /* fast, click-free attack per note */
#define AEC_CHIME_RELEASE_MS    18      /* fade-out tail per note */
#define AEC_CHIME_NOTE_GAP_MS   30      /* short silence between notes */
#define AEC_CHIME_PEAK        3400      /* peak amplitude before envelope (soft) */
#define AEC_CHIME_DECAY       3.2f      /* exponential decay across a note */
#define AEC_CHIME_H2_GAIN     0.33f     /* 2nd harmonic (octave) level */
#define AEC_CHIME_H3_GAIN     0.12f     /* 3rd harmonic level */
#define AEC_CHIME_NORM        (1.0f + AEC_CHIME_H2_GAIN + AEC_CHIME_H3_GAIN)
#define AEC_BEEP_DISCARD_MS    300      /* discard mic after start-chime so the tone tail is not recorded.
                                        * Increased from 200 to 300ms: after playback-abort the I²S TX DMA
                                        * still holds residual chime samples that keep playing from the speaker,
                                        * so we need more discard frames to clear both DMA + acoustic tail. */

/* VAD + OPUS framing. VAD runs on 8kHz downsampled audio (lighter workload). It now
 * decides REC stop: once speech is heard, AEC_VAD_SILENCE_END_MS of trailing silence
 * ends the recording. OPUS encodes the 16kHz rec_buf in batch AFTER recording (ENCODE
 * state) -- real-time encode (~28ms/20ms frame) was a REC bottleneck, so it stays
 * deferred. */
#define VAD_SAMPLE_RATE_HZ     8000
#define VAD_FRAME_MS           20
#define VAD_FRAME_SAMPLES       (VAD_SAMPLE_RATE_HZ * VAD_FRAME_MS / 1000)        /* 160 @8kHz */
#define VAD_BUF_SAMPLES        (VAD_FRAME_SAMPLES * 2)                             /* 320 */
#define OPUS_FRAME_SAMPLES     (AI_MIRROR_SAMPLE_RATE * VAD_FRAME_MS / 1000)      /* 320 @16kHz */
#define OPUS_OUT_BYTES         400
#define AEC_OPUS_MAX_FRAMES    (AEC_REC_BUF_SAMPLES / OPUS_FRAME_SAMPLES)          /* max opus frames in an 8s recording (400) */
/* sdkconfig uses a 100 Hz FreeRTOS tick, so pdMS_TO_TICKS(1) rounds to zero
 * and only yields without letting IDLE1 run. Use a real one-tick delay. */
#define OPUS_TASK_YIELD_MS     10

/* Smart-assistant style chime motifs. A bell-like timbre (fundamental + two
 * phase-locked harmonics behind a percussive decay envelope) and a rising /
 * falling perfect-fifth interval make a gentle "AI listening" cue instead of
 * a raw sine beep. */
typedef struct {
    float freq_hz;
    uint32_t duration_ms;
} aec_chime_note_t;

typedef enum {
    AUDIO_STATE_IDLE = 0,
    AUDIO_STATE_RECORDING = 1,
    AUDIO_STATE_WAIT_ENCODING = 2,
    AUDIO_STATE_WAIT_REPLY = 3,
    AUDIO_STATE_PLAYBACK = 4,
    AUDIO_STATE_SESSION_LISTEN = 5,
} ai_mirror_audio_state_t;

typedef struct {
    int16_t *samples;
    size_t capacity;
    size_t count;
    size_t write_index;
} ai_mirror_preroll_t;

static void aec_preroll_reset(ai_mirror_preroll_t *preroll)
{
    preroll->count = 0;
    preroll->write_index = 0;
}

static void aec_preroll_push(ai_mirror_preroll_t *preroll,
                             const int16_t *samples,
                             size_t sample_count)
{
    for (size_t i = 0; i < sample_count; i++) {
        preroll->samples[preroll->write_index] = samples[i];
        preroll->write_index = (preroll->write_index + 1) % preroll->capacity;
        if (preroll->count < preroll->capacity) {
            preroll->count++;
        }
    }
}

static size_t aec_preroll_copy(const ai_mirror_preroll_t *preroll,
                               int16_t *output,
                               size_t output_capacity)
{
    size_t count = preroll->count < output_capacity
                       ? preroll->count
                       : output_capacity;
    size_t start = (preroll->write_index + preroll->capacity - count) %
                   preroll->capacity;
    for (size_t i = 0; i < count; i++) {
        output[i] = preroll->samples[(start + i) % preroll->capacity];
    }
    return count;
}

/* press: E5 -> B5, rising fifth = "I'm listening" */
static const aec_chime_note_t s_chime_start[] = {
    { 659.25f, 90 },
    { 987.77f, 170 },
};

/* release: B5 -> E5, falling fifth = "conversation ended" */
static const aec_chime_note_t s_chime_end[] = {
    { 987.77f, 90 },
    { 659.25f, 170 },
};

static inline uint32_t aec_mean_abs(const int16_t *data, int n)
{
    uint32_t acc = 0;
    for (int i = 0; i < n; i++) {
        int16_t v = data[i];
        acc += (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
    }
    return acc / n;
}

/* Per-note bell envelope: raised-cosine attack, exponential decay body and a
 * raised-cosine release tail -> gentle, percussive and click-free. */
static float aec_chime_envelope(uint32_t n, uint32_t total)
{
    const uint32_t attack = AI_MIRROR_SAMPLE_RATE * AEC_CHIME_ATTACK_MS / 1000;
    const uint32_t release = AI_MIRROR_SAMPLE_RATE * AEC_CHIME_RELEASE_MS / 1000;
    float env;

    if (n < attack) {
        env = 0.5f * (1.0f - cosf((float)M_PI * (float)n / (float)attack));
    } else {
        env = expf(-AEC_CHIME_DECAY * (float)(n - attack) / (float)(total - attack));
    }
    if (n > total - release) {
        env *= 0.5f * (1.0f + cosf((float)M_PI * (float)(n - (total - release)) / (float)release));
    }
    return env;
}

/* Play a bell-like chime motif as recording feedback. Frequencies stay in the
 * small speaker's efficient range. Blocks the AEC task (owns tx/rx) for the
 * whole motif. scratch must hold AEC_FRAME_BYTES bytes. */
static void play_chime(i2s_chan_handle_t tx, i2s_chan_handle_t rx,
                       int16_t *scratch, const aec_chime_note_t *notes, size_t note_count)
{
    const float two_pi = 2.0f * (float)M_PI;
    const uint32_t gap = AI_MIRROR_SAMPLE_RATE * AEC_CHIME_NOTE_GAP_MS / 1000;

    for (size_t k = 0; k < note_count; k++) {
        const uint32_t total = AI_MIRROR_SAMPLE_RATE * notes[k].duration_ms / 1000;
        const uint32_t slot = (k + 1 < note_count) ? total + gap : total;
        const float phase_inc = two_pi * notes[k].freq_hz / (float)AI_MIRROR_SAMPLE_RATE;
        float phase = 0.0f;
        uint32_t n = 0;

        while (n < slot) {
            for (int j = 0; j < AEC_FRAME_SAMPLES; j++) {
                int16_t sv = 0;
                if (n < total) {
                    /* fundamental + two phase-locked harmonics -> bell timbre */
                    float v = sinf(phase) +
                              AEC_CHIME_H2_GAIN * sinf(2.0f * phase) +
                              AEC_CHIME_H3_GAIN * sinf(3.0f * phase);
                    v *= (float)AEC_CHIME_PEAK / AEC_CHIME_NORM * aec_chime_envelope(n, total);
                    sv = (int16_t)v;
                    phase += phase_inc;
                    if (phase >= two_pi) {
                        phase -= two_pi;
                    }
                }
                scratch[2 * j] = sv;
                scratch[2 * j + 1] = sv;
                n++;
            }
            size_t bw = 0;
            if (i2s_channel_write(tx, scratch, AEC_FRAME_BYTES, &bw, 1000) != ESP_OK) {
                break;
            }
            size_t br = 0;
            i2s_channel_read(rx, scratch, AEC_FRAME_BYTES, &br, 1000);
        }
    }
}

/* Recording-start feedback: play the "start listening" chime, then discard
 * the mic frames that still carry the chime tail so it is not recorded.
 * After playback-abort the I²S TX DMA may still hold residual audio samples
 * from the previous playback session; we write silence to TX during the
 * discard phase to flush those out, ensuring no chime tail bleeds into the
 * recording. ref_ster / mic_ster / chime_buf must each hold AEC_FRAME_BYTES
 * bytes. */
static void aec_rec_start_feedback(i2s_chan_handle_t tx, i2s_chan_handle_t rx,
                                   int16_t *ref_ster, int16_t *mic_ster, int16_t *chime_buf)
{
    play_chime(tx, rx, chime_buf, s_chime_start,
               sizeof(s_chime_start) / sizeof(s_chime_start[0]));
    ESP_LOGI(TAG, "[aec] start chime done");

    const int discard_frames = (AEC_BEEP_DISCARD_MS + AEC_FRAME_MS - 1) / AEC_FRAME_MS;
    /* Write silence to TX to flush any residual DMA data, and read RX to
     * discard the chime tail captured by the microphone. */
    memset(ref_ster, 0, AEC_FRAME_BYTES);
    for (int d = 0; d < discard_frames; d++) {
        size_t bw = 0;
        i2s_channel_write(tx, ref_ster, AEC_FRAME_BYTES, &bw, 1000);
        size_t br = 0;
        i2s_channel_read(rx, mic_ster, AEC_FRAME_BYTES, &br, 1000);
    }
    ESP_LOGI(TAG, "[aec] discarded %d frames (%d ms) after chime", discard_frames, discard_frames * AEC_FRAME_MS);
}

/* Long-conversation end feedback. Drain the I2S/acoustic tail before returning
 * to WakeNet so the device's own chime cannot immediately trigger a new session. */
static void aec_session_end_feedback(i2s_chan_handle_t tx,
                                     i2s_chan_handle_t rx,
                                     int16_t *chime_buf)
{
    play_chime(tx, rx, chime_buf, s_chime_end,
               sizeof(s_chime_end) / sizeof(s_chime_end[0]));

    const int discard_frames =
        (AEC_BEEP_DISCARD_MS + AEC_FRAME_MS - 1) / AEC_FRAME_MS;
    for (int d = 0; d < discard_frames; d++) {
        size_t bw = 0;
        memset(chime_buf, 0, AEC_FRAME_BYTES);
        i2s_channel_write(tx, chime_buf, AEC_FRAME_BYTES, &bw, 1000);
        size_t br = 0;
        i2s_channel_read(rx, chime_buf, AEC_FRAME_BYTES, &br, 1000);
    }
    ESP_LOGI(TAG, "[session] end chime done");
}

/* OPUS encode context shared between the recording task and the async
 * opus_enc_task. The recorder pushes opus-frame sample offsets into a queue
 * while recording; opus_enc_task drains it, encodes, and signals done. */
typedef struct {
    OpusEncoder *enc;
    OpusDecoder *dec;
    int16_t *rec_buf;
    uint8_t *packed;
    int16_t *frame_bytes;
    uint8_t *out;
    int16_t *decode_buf;
    volatile size_t packed_len;
    volatile uint32_t frame_cnt;
    uint32_t total_bytes;
    volatile bool drop_tail;        /* drop trailing ~0.9s on VAD end (set by recorder, read by enc_task) */
    volatile size_t decode_samples; /* decoded PCM samples (written by enc_task, read by recorder for PLAY) */
    volatile int32_t decode_peak;   /* peak of decode_buf (for play_gain) */
    volatile int64_t enc_done_us;   /* timestamp when encoding finished (for enc/dec timing split) */
    QueueHandle_t queue;
    SemaphoreHandle_t done_sem;
} opus_enc_ctx_t;

static opus_enc_ctx_t s_enc;

/* Async opus encoder task: lower priority than the recorder (prio 5) so it
 * never starves capture. It is pinned to CPU1; the recorder/AEC task also
 * runs there, so the encoder must explicitly yield while draining a full
 * queue. Without a yield, IDLE1 can be starved long enough to trip the task
 * watchdog even though the encode itself is making progress. */
static void opus_enc_task(void *args)
{
    (void)args;
    int offset;
    uint32_t frames_since_yield = 0;
    while (1) {
        if (xQueueReceive(s_enc.queue, &offset, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (offset < 0) {
            /* end marker: encoding finished. Now decode packed -> decode_buf on this same
             * CPU1 task (encode and decode are mutually exclusive -- they never overlap),
             * then signal done so the recorder can play. */
            s_enc.enc_done_us = esp_timer_get_time();
            size_t dec_limit = s_enc.frame_cnt;
            if (s_enc.drop_tail && dec_limit >= AEC_VAD_TAIL_DROP_FRAMES) {
                dec_limit -= AEC_VAD_TAIL_DROP_FRAMES;
            }
            size_t dec_byte_off = 0;
            size_t dec_samples = 0;
            int32_t peak = 1;
            if (s_enc.dec && s_enc.decode_buf) {
                for (size_t i = 0; i < dec_limit; i++) {
                    int dec_n = opus_decode(s_enc.dec, s_enc.packed + dec_byte_off,
                                            s_enc.frame_bytes[i],
                                            s_enc.decode_buf + i * OPUS_FRAME_SAMPLES,
                                            OPUS_FRAME_SAMPLES, 0);
                    if (dec_n > 0) {
                        for (int k = 0; k < dec_n; k++) {
                            int32_t v = s_enc.decode_buf[i * OPUS_FRAME_SAMPLES + k];
                            int32_t a = (v < 0) ? -v : v;
                            if (a > peak) {
                                peak = a;
                            }
                        }
                        dec_samples += dec_n;
                    }
                    dec_byte_off += s_enc.frame_bytes[i];
                    if ((i % 10) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(OPUS_TASK_YIELD_MS));
                    }
                    if ((i % 25) == 0) {
                        ESP_LOGI(TAG, "[aec] enc_task dec f=%u/%u samples=%u",
                                 (unsigned)i, (unsigned)dec_limit, (unsigned)dec_samples);
                    }
                }
            }
            s_enc.decode_samples = dec_samples;
            s_enc.decode_peak = peak;
            ESP_LOGI(TAG, "[aec] enc_task end-marker: dec %u/%u frames %u samples, give sem",
                     (unsigned)dec_limit, (unsigned)s_enc.frame_cnt, (unsigned)dec_samples);
            xSemaphoreGive(s_enc.done_sem);
            continue;
        }
        if (s_enc.frame_cnt < AEC_OPUS_MAX_FRAMES &&
            s_enc.packed_len + OPUS_OUT_BYTES <= (size_t)AEC_OPUS_MAX_FRAMES * OPUS_OUT_BYTES) {
            opus_int32 enc_bytes = opus_encode(s_enc.enc, s_enc.rec_buf + offset,
                                               OPUS_FRAME_SAMPLES, s_enc.out, OPUS_OUT_BYTES);
            if (enc_bytes > 0) {
                memcpy(s_enc.packed + s_enc.packed_len, s_enc.out, (size_t)enc_bytes);
                s_enc.frame_bytes[s_enc.frame_cnt] = (int16_t)enc_bytes;
                s_enc.packed_len += enc_bytes;
                s_enc.total_bytes += enc_bytes;
                s_enc.frame_cnt++;
                /* xQueueReceive() returns immediately while the recorder has
                 * queued data. Yield periodically so CPU1's idle task and
                 * lower-priority housekeeping can run during long recordings.
                 * A one-tick delay every four frames adds only a small amount
                 * of latency while preventing the IDLE1 task watchdog from
                 * being starved by opus_encode(). */
                if (++frames_since_yield >= 4) {
                    frames_since_yield = 0;
                    vTaskDelay(pdMS_TO_TICKS(OPUS_TASK_YIELD_MS));
                }
                if ((s_enc.frame_cnt % 25) == 0) {
                    ESP_LOGI(TAG, "[aec] enc_task f=%u packed=%uB", (unsigned)s_enc.frame_cnt, (unsigned)s_enc.total_bytes);
                }
            }
        }
    }
}

static void i2s_aec_demo(void *args)
{
    ESP_LOGI(TAG, "[aec] task entered, calling aec_pro_create");
    const int aec_mode = 2; /* 0=mild,1/2=medium,3/4=aggressive,5=aggressive+S3 accel */
    aec_handle_t aec = aec_pro_create(AEC_FRAME_MS, 1, aec_mode);
    if (!aec) {
        ESP_LOGE(TAG, "[aec] aec_pro_create failed, abort task");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "[aec] AEC created (mode %d, %dms frame)", aec_mode, AEC_FRAME_MS);

    /* Wake word detection (Hi Lexin / wn9_hilexin), loaded from flash "model"
     * partition via esp_srmodel. Runs in IDLE and SESSION_LISTEN so it can start
     * or restart a hands-free conversation. Graceful degradation: if any step
     * fails the device falls back to BTN1-only recording (the existing behavior). */
    const esp_wn_iface_t *wn_iface = NULL;
    model_iface_data_t *wn_model = NULL;
    int wn_chunksize = 0;
    int16_t *wn_ring = NULL;
    int wn_ring_len = 0;
    ESP_LOGI(TAG, "[wn] internal free before init: %u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    srmodel_list_t *sr_models = esp_srmodel_init("model");
    if (sr_models) {
        char *wn_name = esp_srmodel_filter(sr_models, ESP_WN_PREFIX, NULL); /* ESP_WN_PREFIX="wn" */
        if (wn_name) {
            wn_iface = esp_wn_handle_from_name(wn_name);
            if (wn_iface) {
                wn_model = wn_iface->create(wn_name, DET_MODE_90);
                if (wn_model) {
                    wn_chunksize = wn_iface->get_samp_chunksize(wn_model);
                    wn_ring = heap_caps_malloc(wn_chunksize * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                    if (wn_ring) {
                        ESP_LOGI(TAG, "[wn] wakenet ready: %s, chunksize=%d rate=%d (internal free after=%u)",
                                 wn_name, wn_chunksize, wn_iface->get_samp_rate(wn_model),
                                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                    } else {
                        ESP_LOGW(TAG, "[wn] wn_ring alloc failed, wakenet disabled (BTN1-only)");
                        wn_iface->destroy(wn_model);
                        wn_model = NULL;
                        wn_iface = NULL;
                        wn_chunksize = 0;
                    }
                } else {
                    ESP_LOGW(TAG, "[wn] wakenet create failed (BTN1-only)");
                    wn_iface = NULL;
                }
            } else {
                ESP_LOGW(TAG, "[wn] no wakenet handle for '%s' (BTN1-only)", wn_name);
            }
        } else {
            ESP_LOGW(TAG, "[wn] no wakenet model in partition (BTN1-only)");
        }
    } else {
        ESP_LOGW(TAG, "[wn] esp_srmodel_init failed (BTN1-only recording)");
    }

    /* VAD + OPUS encoder: voice-activity detection + compressed encoding demo.
     * VAD/OPUS use 20ms (320 samples @16kHz) frames; AEC uses 16ms (256), so we
     * accumulate AEC-cleaned audio into a ring buffer and feed 20ms slices. */
    vad_handle_t vad = vad_create(VAD_MODE_3);
    int opus_err = 0;
    OpusEncoder *opus_enc = opus_encoder_create(AI_MIRROR_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_err);
    if (opus_enc && opus_err == OPUS_OK) {
        opus_encoder_ctl(opus_enc, OPUS_SET_BITRATE(30000)); /* 30 kbps, voice-grade */
        opus_encoder_ctl(opus_enc, OPUS_SET_COMPLEXITY(3)); /* lower complexity -> faster encode on S3 */
        ESP_LOGI(TAG, "[aec] VAD+OPUS ready (opus 30kbps, complexity 3)");
    } else {
        ESP_LOGE(TAG, "[aec] opus_encoder_create failed err=%d (VAD still runs)", opus_err);
    }
    int opus_dec_err = 0;
    OpusDecoder *opus_dec = opus_decoder_create(AI_MIRROR_SAMPLE_RATE, 1, &opus_dec_err);
    if (!opus_dec || opus_dec_err != OPUS_OK) {
        ESP_LOGE(TAG, "[aec] opus_decoder_create failed err=%d (decode disabled)", opus_dec_err);
    }

    int16_t *mic_ster = heap_caps_malloc(AEC_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    int16_t *ref_ster = heap_caps_malloc(AEC_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    int16_t *mic_mono = heap_caps_malloc(AEC_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *ref_mono = heap_caps_malloc(AEC_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *out_mono = heap_caps_malloc(AEC_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *play_ster = heap_caps_malloc(AEC_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    int16_t *rec_buf = heap_caps_malloc(AEC_REC_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *decode_buf = heap_caps_malloc(AEC_REC_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM); /* opus-decoded PCM for playback */
    int16_t *chime_buf = heap_caps_malloc(AEC_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    int16_t *vad_buf = heap_caps_malloc(VAD_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM); /* ring buf 2x160 @8kHz */
    int16_t *ds_buf = heap_caps_malloc(AEC_FRAME_SAMPLES / 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM); /* 16k->8k downsample scratch */
    int16_t *session_preroll_buf = heap_caps_malloc(
        AEC_SESSION_PREROLL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    uint8_t *opus_out = heap_caps_malloc(OPUS_OUT_BYTES, MALLOC_CAP_SPIRAM); /* single-frame OPUS encode scratch */
    uint8_t *opus_packed = heap_caps_malloc(AEC_OPUS_MAX_FRAMES * OPUS_OUT_BYTES, MALLOC_CAP_SPIRAM); /* packed OPUS frames for decode */
    int16_t *opus_frame_bytes = heap_caps_malloc(AEC_OPUS_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM); /* per-frame encoded byte count */

    if (!mic_ster || !ref_ster || !mic_mono || !ref_mono || !out_mono || !play_ster || !rec_buf || !decode_buf || !chime_buf || !vad_buf || !ds_buf || !session_preroll_buf || !opus_out || !opus_packed || !opus_frame_bytes) {
        ESP_LOGE(TAG, "[aec] no PSRAM for buffers, abort task");
        vTaskDelete(NULL);
    }

    /* Share opus encode resources with the async opus_enc_task. The recorder
     * (this task) pushes opus-frame offsets while recording; opus_enc_task
     * encodes them in the background and signals done. */
    s_enc.enc = opus_enc;
    s_enc.dec = opus_dec;
    s_enc.rec_buf = rec_buf;
    s_enc.packed = opus_packed;
    s_enc.frame_bytes = opus_frame_bytes;
    s_enc.out = opus_out;
    s_enc.decode_buf = decode_buf;
    s_enc.packed_len = 0;
    s_enc.frame_cnt = 0;
    s_enc.total_bytes = 0;
    s_enc.drop_tail = false;
    s_enc.decode_samples = 0;
    s_enc.decode_peak = 1;
    /* Queue storage is not touched by DMA/ISR, so keep it in PSRAM.  The
     * WakeNet/AEC stack leaves only a few KiB of internal RAM at this point. */
    s_enc.queue = xQueueCreateWithCaps(
        AEC_OPUS_MAX_FRAMES, sizeof(int), MALLOC_CAP_SPIRAM);
    s_enc.done_sem = xSemaphoreCreateBinary();
    if (s_enc.queue && s_enc.done_sem) {
        TaskHandle_t enc_handle = NULL;
        BaseType_t tret = xTaskCreatePinnedToCoreWithCaps(opus_enc_task, "opus_enc", 16384, NULL, 4, &enc_handle, 1, MALLOC_CAP_SPIRAM);
        if (tret == pdPASS) {
            ESP_LOGI(TAG, "[aec] async opus encoder task started (prio 4, CPU1, stack 16k PSRAM)");
        } else {
            ESP_LOGE(TAG, "[aec] failed to create opus_enc task ret=%d", (int)tret);
        }
    } else {
        ESP_LOGE(TAG, "[aec] failed to create opus enc queue/sem, encoding disabled");
    }

    ESP_LOGI(TAG, "[aec] demo ready: press BTN1 to record (AEC OFF, raw mic); VAD stops REC; captured PCM uploads by WebSocket; local playback disabled");


    ai_mirror_audio_state_t state = AUDIO_STATE_IDLE;
    size_t rec_samples = 0;
    uint32_t frame_cnt = 0;
    uint32_t idle_cnt = 0;
    float play_gain = 1.0f;
    int64_t rec_start_us = 0;
    int64_t frame_start_us = 0;
    int64_t enc_start_us = 0;      /* encode start timestamp (pushed end-marker); for enc timing */
    bool btn_prev = false;         /* BTN1 level last frame (press-edge trigger) */
    bool wake_triggered = false;   /* set by wakenet in IDLE, consumed as a REC trigger */
    ai_mirror_playback_request_t playback = {0};
    bool session_active = false;
    bool gateway_stream_active = false;
    bool gateway_stream_failed = false;
    char session_id[40] = {0};
    uint32_t session_turn_index = 0;
    int64_t session_deadline_us = 0;
    int64_t reply_deadline_us = 0;
    int64_t session_guard_until_us = 0;
    uint32_t session_vad_speech_run = 0;
    uint64_t session_vad_level_sum = 0;
    uint64_t session_guard_level_sum = 0;
    uint32_t session_guard_level_frames = 0;
    uint32_t session_speech_level = AEC_SESSION_MIN_LEVEL;
    ai_mirror_preroll_t session_preroll = {
        .samples = session_preroll_buf,
        .capacity = AEC_SESSION_PREROLL_SAMPLES,
    };

    /* VAD + OPUS state. opus encode counters live in s_enc (written by
     * opus_enc_task); opus_off is maintained by this recorder task. */
    int vad_len = 0;
    uint32_t vad_speech_cnt = 0, vad_silence_cnt = 0;
    bool vad_heard_speech = false; /* set once VAD detects speech during this REC */
    uint32_t vad_silence_run = 0;  /* consecutive VAD silence frames since last speech */
    size_t opus_off = 0;          /* next opus frame's sample offset in rec_buf (pushed to enc queue) */

    while (1) {
        /* BTN1 press-edge: sampled once per frame so IDLE triggers on a fresh press
         * (no need to hold). btn_prev is updated at frame end (PLAY has its own loop). */
        bool btn_now = board_button_is_pressed(BOARD_BUTTON_ID_1);
        bool btn_press_edge = btn_now && !btn_prev;

        bool can_take_end_conversation =
            !session_active || state == AUDIO_STATE_WAIT_REPLY ||
            state == AUDIO_STATE_SESSION_LISTEN;
        if (can_take_end_conversation &&
            ai_mirror_gateway_take_end_conversation()) {
            if (session_active) {
                ESP_LOGI(TAG, "[session] voice exit command received");
                ai_mirror_gateway_report_session(
                    session_id, "conversation_session_voice_ended",
                    session_turn_index);
                aec_session_end_feedback(tx_handle, rx_handle, chime_buf);
                (void)ai_mirror_gateway_take_continue_listening();
                session_active = false;
                session_id[0] = '\0';
                state = AUDIO_STATE_IDLE;
                idle_cnt = 0;
                vad_len = 0;
                vad_heard_speech = false;
                vad_silence_run = 0;
                session_vad_speech_run = 0;
                session_vad_level_sum = 0;
                session_guard_level_sum = 0;
                session_guard_level_frames = 0;
                aec_preroll_reset(&session_preroll);
                wn_ring_len = 0;
                wake_triggered = false;
                btn_prev = btn_now;
                ESP_LOGI(TAG,
                         "[session] voice exit completed; wake-word mode active");
                continue;
            }
            ESP_LOGW(TAG, "[session] ignored stale end-conversation command");
        }

        /* A playback response must take priority over a stale continue-listening
         * command. SESSION_LISTEN is included so a delayed response cannot wait
         * for the 60-second session timeout before it starts playing. */
        if ((state == AUDIO_STATE_IDLE || state == AUDIO_STATE_WAIT_REPLY ||
             state == AUDIO_STATE_SESSION_LISTEN) &&
            ai_mirror_audio_playback_take(&playback)) {
            play_gain = 1.0f;
            state = AUDIO_STATE_PLAYBACK;
        }

        if (state == AUDIO_STATE_WAIT_REPLY && session_active &&
            ai_mirror_gateway_take_continue_listening()) {
            state = AUDIO_STATE_SESSION_LISTEN;
            session_guard_until_us = esp_timer_get_time() +
                                     (int64_t)AEC_SESSION_GUARD_MS * 1000;
            session_deadline_us = esp_timer_get_time() +
                                  (int64_t)AEC_SESSION_TIMEOUT_MS * 1000;
            vad_len = 0;
            vad_heard_speech = false;
            vad_silence_run = 0;
            session_vad_speech_run = 0;
            session_vad_level_sum = 0;
            session_guard_level_sum = 0;
            session_guard_level_frames = 0;
            session_speech_level = AEC_SESSION_MIN_LEVEL;
            aec_preroll_reset(&session_preroll);
            ESP_LOGI(TAG, "[session] empty/noisy turn ignored; listening resumed");
        }

        /* ---- PLAYBACK: play peak-normalized OPUS-decoded audio, then go idle.
         * BTN1 or wake word aborts playback instantly, drops the recording and goes
         * straight back to REC. While playing, mic is captured and fed through AEC
         * to remove the playback echo, then to WakeNet for hands-free interruption. ---- */
        if (state == AUDIO_STATE_PLAYBACK) {
            ESP_LOGI(TAG, "[aec] LLM playback start, turn=%s %u samples (%u ms)%s",
                     playback.turn_id,
                     (unsigned)playback.sample_count,
                     (unsigned)(playback.sample_count * 1000U / AI_MIRROR_SAMPLE_RATE),
                     playback.streaming ? " [streaming]" : "");
            int64_t playback_started_us = esp_timer_get_time();
            ai_mirror_gateway_report_playback(
                playback.command_id, "playback_started", 0, "");
            ESP_LOGI(TAG, "[aec] LLM playback gain=x%.2f",
                     (double)play_gain);
            size_t played = 0;
            bool aborted = false;
            bool wake_word_aborted = false;
            bool failed = false;
            ai_mirror_playback_chunk_t stream_chunk = {0};
            size_t stream_chunk_offset = 0;
            bool stream_chunk_valid = false;
            wn_ring_len = 0;  /* reset wakenet ring so leftover IDLE audio doesn't trigger */
            while (playback.streaming || played < playback.sample_count) {
                /* Poll BTN1 every frame (~16ms) so playback can be interrupted */
                if (board_button_is_pressed(BOARD_BUTTON_ID_1)) {
                    ESP_LOGI(TAG, "[aec] playback aborted by BTN1, discarding recording");
                    aborted = true;
                    break;
                }
                size_t n = AEC_FRAME_SAMPLES;
                bool have_samples = true;
                if (playback.streaming) {
                    if (!stream_chunk_valid || stream_chunk_offset >= stream_chunk.sample_count) {
                        if (stream_chunk_valid) {
                            ai_mirror_audio_playback_stream_release_chunk(&stream_chunk);
                        }
                        stream_chunk_offset = 0;
                        stream_chunk_valid = ai_mirror_audio_playback_stream_take_chunk(&stream_chunk);
                    }
                    if (!stream_chunk_valid) {
                        if (ai_mirror_audio_playback_stream_end_received() ||
                            ai_mirror_audio_playback_stream_has_error()) {
                            break;
                        }
                        /* Keep I2S/AEC alive while the next network frame is in flight. */
                        have_samples = false;
                    } else if (stream_chunk_offset + n > stream_chunk.sample_count) {
                        n = stream_chunk.sample_count - stream_chunk_offset;
                    }
                } else if (played + n > playback.sample_count) {
                    n = playback.sample_count - played;
                }
                /* Build stereo playback frame */
                for (size_t j = 0; j < n; j++) {
                    int16_t source_sample = 0;
                    if (have_samples) {
                        source_sample = playback.streaming
                            ? stream_chunk.samples[stream_chunk_offset + j]
                            : playback.samples[played + j];
                    }
                    float fv = (float)source_sample * play_gain;
                    if (fv > 32767.0f) {
                        fv = 32767.0f;
                    }
                    if (fv < -32768.0f) {
                        fv = -32768.0f;
                    }
                    int16_t sv = (int16_t)fv;
                    play_ster[2 * j] = sv;
                    play_ster[2 * j + 1] = sv;
                }
                /* Zero-pad remaining samples if last frame is short */
                for (size_t j = n; j < AEC_FRAME_SAMPLES; j++) {
                    play_ster[2 * j] = 0;
                    play_ster[2 * j + 1] = 0;
                }

                /* Write audio to speaker AND simultaneously read mic from I²S.
                 * The blocking write + read gives us a full-duplex frame: speaker
                 * plays while mic captures, so AEC has a real reference to cancel. */
                size_t bw = 0, br = 0;
                esp_err_t ret = i2s_channel_write(tx_handle, play_ster, AEC_FRAME_BYTES, &bw, 1000);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "[aec] playback write failed: %s", esp_err_to_name(ret));
                    failed = true;
                    break;
                }
                /* Read mic during playback for wake-word detection */
                ret = i2s_channel_read(rx_handle, mic_ster, AEC_FRAME_BYTES, &br, 1000);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "[aec] playback mic read failed: %s", esp_err_to_name(ret));
                } else {
                    /* Extract mono mic (left channel) */
                    for (int j = 0; j < AEC_FRAME_SAMPLES; j++) {
                        mic_mono[j] = mic_ster[2 * j];
                    }
                    /* Build mono reference from what we just played */
                    for (int j = 0; j < AEC_FRAME_SAMPLES; j++) {
                        ref_mono[j] = play_ster[2 * j];
                    }
                    /* Run AEC: remove playback echo from mic signal */
                    aec_process(aec, mic_mono, ref_mono, out_mono);

                    /* Feed AEC-cleaned audio to WakeNet for hands-free interrupt */
                    if (wn_iface && wn_model && wn_ring) {
                        int copied = 0;
                        while (copied < AEC_FRAME_SAMPLES) {
                            int space = wn_chunksize - wn_ring_len;
                            int cn = AEC_FRAME_SAMPLES - copied;
                            if (cn > space) {
                                cn = space;
                            }
                            memcpy(wn_ring + wn_ring_len, out_mono + copied, cn * sizeof(int16_t));
                            wn_ring_len += cn;
                            copied += cn;
                            while (wn_ring_len >= wn_chunksize) {
                                wakenet_state_t wr = wn_iface->detect(wn_model, wn_ring);
                                if (wr == WAKENET_DETECTED) {
                                    ESP_LOGI(TAG, "[wn] wake word detected during playback, aborting playback");
                                    aborted = true;
                                    wake_word_aborted = true;
                                    break;
                                }
                                memmove(wn_ring, wn_ring + wn_chunksize,
                                        (wn_ring_len - wn_chunksize) * sizeof(int16_t));
                                wn_ring_len -= wn_chunksize;
                            }
                            if (aborted) {
                                break;
                            }
                        }
                    }
                }
                if (have_samples) {
                    played += n;
                    if (playback.streaming) {
                        stream_chunk_offset += n;
                    }
                }
                if (aborted) {
                    break;
                }
            }
            if (stream_chunk_valid) {
                ai_mirror_audio_playback_stream_release_chunk(&stream_chunk);
            }
            if (playback.streaming && ai_mirror_audio_playback_stream_has_error()) {
                failed = true;
            }
            if (aborted) {
                /* Flush queued playback samples so the speaker goes silent NOW,
                 * then drop the recording and restart recording directly. */
                i2s_channel_disable(tx_handle);
                i2s_channel_enable(tx_handle);
                if (s_enc.queue) { xQueueReset(s_enc.queue); }
                if (s_enc.done_sem) { xSemaphoreTake(s_enc.done_sem, 0); } /* clear stale done */
                wn_ring_len = 0;  /* clear wakenet ring after playback abort */
                uint32_t playback_ms = (uint32_t)((esp_timer_get_time() - playback_started_us) / 1000);
                ai_mirror_gateway_report_playback(
                    playback.command_id, "playback_aborted", playback_ms, "interrupted by user");
                ai_mirror_audio_playback_stream_abort();
                ai_mirror_audio_playback_release(&playback);

                /* A wake word during a long-chat answer starts a fresh
                 * conversation, rather than appending the next utterance to
                 * the interrupted turn. BTN1 keeps the existing same-session
                 * recording behavior. */
                if (wake_word_aborted && session_active) {
                    ai_mirror_gateway_report_session(
                        session_id, "conversation_session_ended", session_turn_index);
                    session_active = false;
                    session_id[0] = '\0';
                    session_turn_index = 0;
                    session_vad_speech_run = 0;
                    session_vad_level_sum = 0;
                    session_guard_level_sum = 0;
                    session_guard_level_frames = 0;
                    session_speech_level = AEC_SESSION_MIN_LEVEL;
                    aec_preroll_reset(&session_preroll);
                    snprintf(session_id, sizeof(session_id), "%08" PRIx32 "%08" PRIx32,
                             esp_random(), esp_random());
                    session_active = true;
                    session_turn_index = 0;
                    ai_mirror_gateway_report_session(
                        session_id, "conversation_session_started", session_turn_index);
                    ESP_LOGI(TAG, "[session] wake word interrupted playback, restarted conversation id=%s",
                             session_id);
                }
                state = AUDIO_STATE_RECORDING;
                rec_samples = 0;
                frame_cnt = 0;
                opus_off = 0;
                vad_heard_speech = false;
                vad_silence_run = 0;
                s_enc.packed_len = 0;
                s_enc.frame_cnt = 0;
                s_enc.total_bytes = 0;
                s_enc.drop_tail = false;
                ESP_LOGI(TAG, "[aec] recording restart (playback interrupted)");
                aec_rec_start_feedback(tx_handle, rx_handle, ref_ster, mic_ster, chime_buf);
                rec_start_us = esp_timer_get_time();
                continue;
            }
            uint32_t playback_ms = (uint32_t)((esp_timer_get_time() - playback_started_us) / 1000);
            if (failed) {
                ai_mirror_gateway_report_playback(
                    playback.command_id, "playback_failed", playback_ms, "I2S write failed");
            } else {
                ai_mirror_gateway_report_playback(
                    playback.command_id, "playback_completed", playback_ms, "");
            }
            ESP_LOGI(TAG, "[aec] LLM playback %s, %u samples %ums",
                     failed ? "failed" : "done", (unsigned)played, (unsigned)playback_ms);
            ai_mirror_audio_playback_release(&playback);
            wn_ring_len = 0;  /* clear wakenet ring for clean IDLE re-entry */
            if (session_active) {
                state = AUDIO_STATE_SESSION_LISTEN;
                session_guard_until_us = esp_timer_get_time() +
                                         (int64_t)AEC_SESSION_GUARD_MS * 1000;
                session_deadline_us = esp_timer_get_time() +
                                      (int64_t)AEC_SESSION_TIMEOUT_MS * 1000;
                vad_len = 0;
                vad_heard_speech = false;
                vad_silence_run = 0;
                session_vad_speech_run = 0;
                session_vad_level_sum = 0;
                session_guard_level_sum = 0;
                session_guard_level_frames = 0;
                session_speech_level = AEC_SESSION_MIN_LEVEL;
                aec_preroll_reset(&session_preroll);
                ai_mirror_gateway_report_session(
                    session_id, "conversation_listening", session_turn_index);
                ESP_LOGI(TAG, "[session] listening for follow-up, timeout=%ums",
                         (unsigned)AEC_SESSION_TIMEOUT_MS);
            } else {
                state = AUDIO_STATE_IDLE;
            }
            idle_cnt = 0;
            continue;
        }

        /* ---- IDLE / REC / WAIT: keep I2S serviced; speaker stays SILENT (no reference tone) ---- */
        memset(ref_mono, 0, AEC_FRAME_SAMPLES * sizeof(int16_t));
        memset(ref_ster, 0, AEC_FRAME_BYTES);

        size_t bw = 0, br = 0;
        /* Non-blocking tx write (silence): dropping is fine, auto_clear keeps speaker silent.
         * Blocking write waited ~16ms per frame and caused ~50% frame drops. */
        (void)i2s_channel_write(tx_handle, ref_ster, AEC_FRAME_BYTES, &bw, 0);
        esp_err_t rret = i2s_channel_read(rx_handle, mic_ster, AEC_FRAME_BYTES, &br, 1000);
        if (rret != ESP_OK) {
            ESP_LOGE(TAG, "[aec] rx read failed: %s", esp_err_to_name(rret));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (int j = 0; j < AEC_FRAME_SAMPLES; j++) {
            mic_mono[j] = mic_ster[2 * j];
        }

        if (state == AUDIO_STATE_RECORDING) { /* REC: AEC OFF, store raw mic + feed VAD + push opus frames to enc task */
            frame_start_us = esp_timer_get_time();

            /* VAD on raw mic (AEC disabled): downsample mic_mono (16k/256s) to 8k
             * (128s) and feed 20ms (160s @8kHz) frames to VAD. REC stop is decided
             * by VAD trailing silence after speech. */
            {
                const int ds_n = AEC_FRAME_SAMPLES / 2;
                for (int j = 0; j < ds_n; j++) {
                    ds_buf[j] = (int16_t)(((int32_t)mic_mono[2 * j] + (int32_t)mic_mono[2 * j + 1]) >> 1);
                }
                int copied = 0;
                while (copied < ds_n) {
                    int space = VAD_BUF_SAMPLES - vad_len;
                    int n = ds_n - copied;
                    if (n > space) n = space;
                    memcpy(vad_buf + vad_len, ds_buf + copied, n * sizeof(int16_t));
                    vad_len += n;
                    copied += n;
                    while (vad_len >= VAD_FRAME_SAMPLES) {
                        vad_state_t vst = vad_process(vad, vad_buf, VAD_SAMPLE_RATE_HZ, VAD_FRAME_MS);
                        if (vst == VAD_SPEECH) {
                            vad_speech_cnt++;
                            vad_heard_speech = true;
                            vad_silence_run = 0;
                        } else {
                            vad_silence_cnt++;
                            if (vad_heard_speech) {
                                vad_silence_run++;
                            }
                        }
                        if ((vad_speech_cnt + vad_silence_cnt) % 25 == 0) {
                            ESP_LOGI(TAG, "[aec] VAD:%s spk=%u sil=%u run=%u",
                                     vst == VAD_SPEECH ? "SPK" : "sil",
                                     (unsigned)vad_speech_cnt, (unsigned)vad_silence_cnt,
                                     (unsigned)vad_silence_run);
                        }
                        memmove(vad_buf, vad_buf + VAD_FRAME_SAMPLES, (vad_len - VAD_FRAME_SAMPLES) * sizeof(int16_t));
                        vad_len -= VAD_FRAME_SAMPLES;
                    }
                }
            }

            size_t room = AEC_REC_BUF_SAMPLES - rec_samples;
            size_t n = (AEC_FRAME_SAMPLES < room) ? AEC_FRAME_SAMPLES : room;
            memcpy(rec_buf + rec_samples, mic_mono, n * sizeof(int16_t));
            rec_samples += n;
            if (gateway_stream_active && !gateway_stream_failed &&
                ai_mirror_gateway_stream_write(mic_mono, n) != ESP_OK) {
                ESP_LOGW(TAG, "[gateway] streaming upload failed; falling back to turn upload");
                gateway_stream_failed = true;
            }

            /* push complete 20ms (320-sample) opus frames to the async encoder task */
            while (rec_samples - opus_off >= OPUS_FRAME_SAMPLES && opus_off + OPUS_FRAME_SAMPLES <= AEC_REC_BUF_SAMPLES) {
                int offset = (int)opus_off;
                if (s_enc.queue && xQueueSend(s_enc.queue, &offset, 0) != pdPASS) {
                    ESP_LOGW(TAG, "[aec] enc queue full, dropping opus frame off=%u", (unsigned)opus_off);
                }
                opus_off += OPUS_FRAME_SAMPLES;
            }

            if ((frame_cnt % 31) == 0) {
                uint32_t mic_lvl = aec_mean_abs(mic_mono, AEC_FRAME_SAMPLES);
                ESP_LOGI(TAG, "[aec] lvl mic=%u (REC, AEC off) frame=%uus rt=16000us", (unsigned)mic_lvl, (unsigned)(esp_timer_get_time() - frame_start_us));
            }
            frame_cnt++;

            /* REC stop is decided by VAD: once speech has been heard, trailing silence
             * of AEC_VAD_SILENCE_END_MS means the user finished talking. Buffer-full is
             * a safety cap. (BTN1 no longer stops REC -- a press only starts it.) */
            bool vad_end = vad_heard_speech &&
                           (vad_silence_run * VAD_FRAME_MS >= AEC_VAD_SILENCE_END_MS);
            if (vad_end || rec_samples >= AEC_REC_BUF_SAMPLES) {
                s_enc.drop_tail = vad_end;  /* drop trailing ~0.9s silence only on VAD end */
                ESP_LOGI(TAG, "[aec] recording stop (%s), %u samples (%u ms)",
                         vad_end ? "vad-silence" : "buf-full",
                         (unsigned)rec_samples,
                         (unsigned)(rec_samples * 1000U / AI_MIRROR_SAMPLE_RATE));
                session_turn_index++;
                esp_err_t gateway_err = ESP_OK;
                if (gateway_stream_active) {
                    gateway_err = ai_mirror_gateway_stream_end();
                    gateway_stream_active = false;
                    gateway_stream_failed = false;
                }
                if (gateway_err != ESP_OK) {
                    gateway_err = ai_mirror_gateway_submit_turn_pcm(
                        rec_buf,
                        rec_samples,
                        AI_MIRROR_SAMPLE_RATE,
                        session_id,
                        session_turn_index);
                }
                if (gateway_err != ESP_OK) {
                    ESP_LOGW(TAG, "[gateway] recording upload skipped: %s", esp_err_to_name(gateway_err));
                }
                {                     int64_t rec_wall_us = esp_timer_get_time() - rec_start_us;                     uint32_t rec_wall_ms = (uint32_t)(rec_wall_us / 1000);                     uint32_t expected_ms = (uint32_t)(rec_samples * 1000U / AI_MIRROR_SAMPLE_RATE);                     ESP_LOGI(TAG, "[aec] diag: rec wall=%ums expected16k=%ums ratio=%.2f", (unsigned)rec_wall_ms, (unsigned)expected_ms, rec_wall_ms ? (double)expected_ms / (double)rec_wall_ms : 0.0);                 }
                /* Push end-marker: opus_enc_task finishes the last queued frames,
                 * then decodes packed -> decode_buf, then signals done. Enter WAIT_ENC.
                 * Playback is intentionally disabled; decode remains as an integrity check. */
                int end_marker = -1;
                if (s_enc.queue) {
                    xQueueSend(s_enc.queue, &end_marker, portMAX_DELAY);
                }
                enc_start_us = esp_timer_get_time();
                state = AUDIO_STATE_WAIT_ENCODING;
            }
        } else if (state == AUDIO_STATE_IDLE) { /* IDLE: silent, feed wakenet + wait for BTN1 press-edge */
            idle_cnt++;
            if ((idle_cnt % 375) == 0) {
                ESP_LOGI(TAG, "[aec] idle (silent), ready - say wake word or press BTN1 to record (stack free=%u)",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }

            /* Feed wake word detector: accumulate this frame's mic_mono into wn_ring
             * and run detect() on each full chunk. WAKENET_DETECTED arms
             * wake_triggered, which triggers REC below exactly like a BTN1 press. */
            if (wn_iface && wn_model && wn_ring) {
                int copied = 0;
                while (copied < AEC_FRAME_SAMPLES) {
                    int space = wn_chunksize - wn_ring_len;
                    int n = AEC_FRAME_SAMPLES - copied;
                    if (n > space) {
                        n = space;
                    }
                    memcpy(wn_ring + wn_ring_len, mic_mono + copied, n * sizeof(int16_t));
                    wn_ring_len += n;
                    copied += n;
                    while (wn_ring_len >= wn_chunksize) {
                        wakenet_state_t wr = wn_iface->detect(wn_model, wn_ring);
                        if (wr == WAKENET_DETECTED) {
                            wake_triggered = true;
                        }
                        memmove(wn_ring, wn_ring + wn_chunksize,
                                (wn_ring_len - wn_chunksize) * sizeof(int16_t));
                        wn_ring_len -= wn_chunksize;
                        if (wake_triggered) {
                            break;
                        }
                    }
                    if (wake_triggered) {
                        break;
                    }
                }
            }

            bool gateway_record_requested = ai_mirror_gateway_take_record_request();
            if (btn_press_edge || wake_triggered || gateway_record_requested) {
                if (wake_triggered) {
                    ESP_LOGI(TAG, "[wn] wake word detected, starting recording");
                } else if (gateway_record_requested) {
                    ESP_LOGI(TAG, "[gateway] remote start-recording command accepted");
                }
                snprintf(session_id, sizeof(session_id), "%08" PRIx32 "%08" PRIx32,
                         esp_random(), esp_random());
                session_active = true;
                session_turn_index = 0;
                ai_mirror_gateway_report_session(
                    session_id, "conversation_session_started", 0);
                gateway_stream_active =
                    ai_mirror_gateway_stream_begin(
                        session_id, AI_MIRROR_SAMPLE_RATE,
                        session_turn_index + 1) == ESP_OK;
                gateway_stream_failed = false;
                ESP_LOGI(TAG, "[session] started id=%s", session_id);
                state = AUDIO_STATE_RECORDING;
                rec_samples = 0;
                frame_cnt = 0;
                opus_off = 0;
                vad_heard_speech = false;
                vad_silence_run = 0;
                /* reset opus encode state + drain any stale queue/done */
                s_enc.packed_len = 0;
                s_enc.frame_cnt = 0;
                s_enc.total_bytes = 0;
                s_enc.drop_tail = false;
                if (s_enc.queue) {
                    xQueueReset(s_enc.queue);
                }
                if (s_enc.done_sem) { xSemaphoreTake(s_enc.done_sem, 0); } /* clear stale done */
                /* reset wakenet ring buffer. NOTE: wn_iface->clean() is NOT called - it
                 * dereferences a NULL conv-queue buffer (a model buffer failed to allocate
                 * under internal-RAM heap fragmentation); detect's streaming state
                 * self-refreshes, and IDLE/SESSION_LISTEN feed wakenet only when
                 * the device is not recording or playing. */
                wake_triggered = false;
                wn_ring_len = 0;
                ESP_LOGI(TAG, "[aec] recording start (AEC off, raw mic) - VAD will stop");
                aec_rec_start_feedback(tx_handle, rx_handle, ref_ster, mic_ster, chime_buf);
                rec_start_us = esp_timer_get_time();
            }
        } else if (state == AUDIO_STATE_SESSION_LISTEN) {
            int64_t now_us = esp_timer_get_time();
            if (now_us >= session_deadline_us) {
                ESP_LOGI(TAG, "[session] idle timeout, returning to wake-word mode");
                ai_mirror_gateway_report_session(
                    session_id, "conversation_session_ended", session_turn_index);
                aec_session_end_feedback(tx_handle, rx_handle, chime_buf);
                session_active = false;
                session_id[0] = '\0';
                state = AUDIO_STATE_IDLE;
                idle_cnt = 0;
                vad_len = 0;
                session_vad_speech_run = 0;
                session_vad_level_sum = 0;
                aec_preroll_reset(&session_preroll);
                wn_ring_len = 0;
            } else if (now_us < session_guard_until_us) {
                session_guard_level_sum +=
                    aec_mean_abs(mic_mono, AEC_FRAME_SAMPLES);
                session_guard_level_frames++;
            } else {
                bool wake_word_detected = false;
                if (session_guard_level_frames > 0) {
                    uint32_t noise_level = (uint32_t)(
                        session_guard_level_sum / session_guard_level_frames);
                    uint32_t dynamic_level =
                        noise_level * AEC_SESSION_NOISE_MULTIPLIER;
                    if (dynamic_level < AEC_SESSION_MIN_LEVEL) {
                        dynamic_level = AEC_SESSION_MIN_LEVEL;
                    }
                    if (dynamic_level > AEC_SESSION_MAX_DYNAMIC_LEVEL) {
                        dynamic_level = AEC_SESSION_MAX_DYNAMIC_LEVEL;
                    }
                    session_speech_level = dynamic_level;
                    ESP_LOGI(TAG,
                             "[session] noise=%u speech-level=%u guard=%ums",
                             (unsigned)noise_level,
                             (unsigned)session_speech_level,
                             (unsigned)AEC_SESSION_GUARD_MS);
                    session_guard_level_frames = 0;
                }

                /* Keep WakeNet active during long-chat listening. A detected
                 * wake word ends the current session and starts a fresh one,
                 * matching the normal IDLE wake-word path. */
                if (wn_iface && wn_model && wn_ring) {
                    int copied = 0;
                    while (copied < AEC_FRAME_SAMPLES) {
                        int space = wn_chunksize - wn_ring_len;
                        int n = AEC_FRAME_SAMPLES - copied;
                        if (n > space) {
                            n = space;
                        }
                        memcpy(wn_ring + wn_ring_len,
                               mic_mono + copied,
                               n * sizeof(int16_t));
                        wn_ring_len += n;
                        copied += n;
                        while (wn_ring_len >= wn_chunksize) {
                            wakenet_state_t wr = wn_iface->detect(wn_model, wn_ring);
                            if (wr == WAKENET_DETECTED) {
                                wake_word_detected = true;
                                break;
                            }
                            memmove(wn_ring,
                                    wn_ring + wn_chunksize,
                                    (wn_ring_len - wn_chunksize) * sizeof(int16_t));
                            wn_ring_len -= wn_chunksize;
                        }
                        if (wake_word_detected) {
                            break;
                        }
                    }
                }

                if (wake_word_detected) {
                    ESP_LOGI(TAG,
                             "[session] wake word detected, restarting conversation");
                    ai_mirror_gateway_report_session(
                        session_id, "conversation_session_ended", session_turn_index);
                    session_active = false;
                    session_id[0] = '\0';
                    session_turn_index = 0;
                    session_vad_speech_run = 0;
                    session_vad_level_sum = 0;
                    session_guard_level_sum = 0;
                    session_guard_level_frames = 0;
                    aec_preroll_reset(&session_preroll);
                    wn_ring_len = 0;

                    snprintf(session_id, sizeof(session_id), "%08" PRIx32 "%08" PRIx32,
                             esp_random(), esp_random());
                    session_active = true;
                    session_turn_index = 0;
                    ai_mirror_gateway_report_session(
                        session_id, "conversation_session_started", 0);
                    ESP_LOGI(TAG, "[session] restarted id=%s", session_id);

                    gateway_stream_active =
                        ai_mirror_gateway_stream_begin(
                            session_id, AI_MIRROR_SAMPLE_RATE,
                            session_turn_index + 1) == ESP_OK;
                    gateway_stream_failed = false;
                    state = AUDIO_STATE_RECORDING;
                    rec_samples = 0;
                    frame_cnt = 0;
                    opus_off = 0;
                    vad_len = 0;
                    vad_heard_speech = false;
                    vad_speech_cnt = 0;
                    vad_silence_cnt = 0;
                    vad_silence_run = 0;
                    s_enc.packed_len = 0;
                    s_enc.frame_cnt = 0;
                    s_enc.total_bytes = 0;
                    s_enc.drop_tail = false;
                    if (s_enc.queue) {
                        xQueueReset(s_enc.queue);
                    }
                    if (s_enc.done_sem) {
                        xSemaphoreTake(s_enc.done_sem, 0);
                    }
                    wake_triggered = false;
                    aec_rec_start_feedback(
                        tx_handle, rx_handle, ref_ster, mic_ster, chime_buf);
                    rec_start_us = esp_timer_get_time();
                    continue;
                }

                aec_preroll_push(
                    &session_preroll, mic_mono, AEC_FRAME_SAMPLES);
                bool speech_triggered = false;
                uint32_t mic_level =
                    aec_mean_abs(mic_mono, AEC_FRAME_SAMPLES);
                const int ds_n = AEC_FRAME_SAMPLES / 2;
                for (int j = 0; j < ds_n; j++) {
                    ds_buf[j] = (int16_t)(((int32_t)mic_mono[2 * j] +
                                           (int32_t)mic_mono[2 * j + 1]) >> 1);
                }
                int copied = 0;
                while (copied < ds_n && !speech_triggered) {
                    int space = VAD_BUF_SAMPLES - vad_len;
                    int n = ds_n - copied;
                    if (n > space) {
                        n = space;
                    }
                    memcpy(vad_buf + vad_len, ds_buf + copied,
                           n * sizeof(int16_t));
                    vad_len += n;
                    copied += n;
                    while (vad_len >= VAD_FRAME_SAMPLES) {
                        vad_state_t vst = vad_process(
                            vad, vad_buf, VAD_SAMPLE_RATE_HZ, VAD_FRAME_MS);
                        memmove(vad_buf,
                                vad_buf + VAD_FRAME_SAMPLES,
                                (vad_len - VAD_FRAME_SAMPLES) * sizeof(int16_t));
                        vad_len -= VAD_FRAME_SAMPLES;
                        if (vst == VAD_SPEECH) {
                            session_vad_speech_run++;
                            session_vad_level_sum += mic_level;
                            if (session_vad_speech_run >=
                                AEC_SESSION_VAD_RUN_FRAMES) {
                                uint32_t speech_level = (uint32_t)(
                                    session_vad_level_sum /
                                    session_vad_speech_run);
                                if (speech_level >= session_speech_level) {
                                    ESP_LOGI(
                                        TAG,
                                        "[session] speech avg=%u threshold=%u",
                                        (unsigned)speech_level,
                                        (unsigned)session_speech_level);
                                    speech_triggered = true;
                                    break;
                                }
                                session_vad_speech_run = 0;
                                session_vad_level_sum = 0;
                            }
                        } else {
                            session_vad_speech_run = 0;
                            session_vad_level_sum = 0;
                        }
                    }
                }

                bool gateway_record_requested =
                    ai_mirror_gateway_take_record_request();
                if (speech_triggered || btn_press_edge || gateway_record_requested) {
                    rec_samples = aec_preroll_copy(
                        &session_preroll, rec_buf, AEC_REC_BUF_SAMPLES);
                    frame_cnt = 0;
                    opus_off = 0;
                    vad_heard_speech = speech_triggered;
                    vad_speech_cnt = speech_triggered ? 1 : 0;
                    vad_silence_cnt = 0;
                    vad_silence_run = 0;
                    vad_len = 0;
                    session_vad_speech_run = 0;
                    session_vad_level_sum = 0;
                    s_enc.packed_len = 0;
                    s_enc.frame_cnt = 0;
                    s_enc.total_bytes = 0;
                    s_enc.drop_tail = false;
                    if (s_enc.queue) {
                        xQueueReset(s_enc.queue);
                    }
                    if (s_enc.done_sem) {
                        xSemaphoreTake(s_enc.done_sem, 0);
                    }
                    state = AUDIO_STATE_RECORDING;
                    gateway_stream_active =
                        ai_mirror_gateway_stream_begin(
                            session_id, AI_MIRROR_SAMPLE_RATE,
                            session_turn_index + 1) == ESP_OK;
                    gateway_stream_failed = false;
                    if (gateway_stream_active && rec_samples > 0 &&
                        ai_mirror_gateway_stream_write(rec_buf, rec_samples) != ESP_OK) {
                        ESP_LOGW(TAG, "[gateway] preroll stream upload failed; using turn fallback");
                        gateway_stream_failed = true;
                    }
                    rec_start_us = esp_timer_get_time() -
                                   (int64_t)rec_samples * 1000000 /
                                       AI_MIRROR_SAMPLE_RATE;
                    ESP_LOGI(TAG,
                             "[session] follow-up detected, preroll=%ums turn=%u",
                             (unsigned)(rec_samples * 1000U /
                                        AI_MIRROR_SAMPLE_RATE),
                             (unsigned)(session_turn_index + 1));
                    aec_preroll_reset(&session_preroll);
                }
            }
        } else if (state == AUDIO_STATE_WAIT_REPLY) {
            if (esp_timer_get_time() >= reply_deadline_us) {
                ESP_LOGW(TAG, "[session] reply timeout, ending session");
                ai_mirror_gateway_report_session(
                    session_id, "conversation_session_reply_timeout",
                    session_turn_index);
                aec_session_end_feedback(tx_handle, rx_handle, chime_buf);
                session_active = false;
                session_id[0] = '\0';
                state = AUDIO_STATE_IDLE;
                idle_cnt = 0;
            }
        } else if (state == AUDIO_STATE_WAIT_ENCODING) { /* WAIT_ENC: wait for async Opus encode/decode integrity check. */
            if (btn_now) {
                ESP_LOGI(TAG, "[aec] wait-enc aborted by BTN1, discarding recording");
                if (s_enc.queue) { xQueueReset(s_enc.queue); }
                if (s_enc.done_sem) { xSemaphoreTake(s_enc.done_sem, 0); } /* clear stale done */
                state = AUDIO_STATE_RECORDING;
                rec_samples = 0;
                frame_cnt = 0;
                opus_off = 0;
                vad_heard_speech = false;
                vad_silence_run = 0;
                s_enc.packed_len = 0;
                s_enc.frame_cnt = 0;
                s_enc.total_bytes = 0;
                s_enc.drop_tail = false;
                aec_rec_start_feedback(tx_handle, rx_handle, ref_ster, mic_ster, chime_buf);
                rec_start_us = esp_timer_get_time();
            } else if (!s_enc.queue || !s_enc.done_sem || xSemaphoreTake(s_enc.done_sem, 0) == pdPASS) {
                /* Encode + decode complete (or no encoder). Do not play the captured audio. */
                int64_t now_us = esp_timer_get_time();
                uint32_t enc_ms = (uint32_t)((s_enc.enc_done_us - enc_start_us) / 1000);
                uint32_t dec_ms = (uint32_t)((now_us - s_enc.enc_done_us) / 1000);
                ESP_LOGI(TAG, "[aec] opus enc+dec done: %u frames %u bytes, dec=%u samples peak=%d (enc=%ums dec=%ums); playback disabled",
                         (unsigned)s_enc.frame_cnt, (unsigned)s_enc.total_bytes,
                         (unsigned)s_enc.decode_samples, (int)s_enc.decode_peak,
                         (unsigned)enc_ms, (unsigned)dec_ms);
                state = session_active ? AUDIO_STATE_WAIT_REPLY : AUDIO_STATE_IDLE;
                reply_deadline_us = esp_timer_get_time() +
                                    (int64_t)AEC_REPLY_TIMEOUT_MS * 1000;
                idle_cnt = 0;
                wn_ring_len = 0;
            } else {
                /* still encoding/decoding: yield CPU to opus_enc_task */
                vTaskDelay(pdMS_TO_TICKS(OPUS_TASK_YIELD_MS));
            }
        }
        btn_prev = btn_now;
    }
}
#endif


/* ================================================================
 *  UI / input / network orchestration
 * ================================================================ */
#define AI_MIRROR_BOOT_PROV_HOLD_MS        2000
#define AI_MIRROR_BOOT_DECISION_WINDOW_MS  3500
#define AI_MIRROR_WIFI_CONNECT_TIMEOUT_MS  20000
#define AI_MIRROR_INTERNET_TIMEOUT_MS      6000
#define AI_MIRROR_LVGL_BUFFER_LINES        10

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
    if (ai_mirror_audio_playback_init() != ESP_OK) {
        ESP_LOGE(TAG, "playback queue init failed, skip audio");
        return false;
    }
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
        ESP_LOGI(TAG, "Start AEC demo");
        ESP_LOGI(TAG, "[aec] internal free heap before xTaskCreate: %u bytes",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        BaseType_t xret = xTaskCreatePinnedToCoreWithCaps(
            i2s_aec_demo, "aec_demo", 3584, NULL, 5, NULL, 1,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (xret != pdPASS) {
            ESP_LOGE(TAG, "[aec] xTaskCreate FAILED (ret=%d), task not started", (int)xret);
        } else {
            ESP_LOGI(TAG, "[aec] xTaskCreate OK, aec_demo task created");
        }
    } else {
        ESP_LOGW(TAG, "Audio disabled, AEC task not started");
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
        esp_err_t gateway_err = ai_mirror_gateway_start();
        if (gateway_err != ESP_OK) {
            ESP_LOGW(TAG, "[gateway] client not started: %s", esp_err_to_name(gateway_err));
        }
    }

    ai_mirror_audio_start(audio_ok);
}
