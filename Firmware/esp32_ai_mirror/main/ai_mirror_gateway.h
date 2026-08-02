#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the background gateway client.
 *
 * The caller must ensure Wi-Fi has been initialized. When gateway support is
 * disabled in menuconfig this function is a no-op that returns ESP_OK.
 */
esp_err_t ai_mirror_gateway_start(void);

/**
 * Copy a mono signed-16 PCM recording into PSRAM and queue it for WebSocket upload.
 * Ownership of the caller's buffer remains with the caller.
 */
esp_err_t ai_mirror_gateway_submit_pcm(const int16_t *samples,
                                       size_t sample_count,
                                       uint32_t sample_rate);

/** Queue one completed conversation turn with session metadata. */
esp_err_t ai_mirror_gateway_submit_turn_pcm(const int16_t *samples,
                                            size_t sample_count,
                                            uint32_t sample_rate,
                                            const char *session_id,
                                            uint32_t turn_index);

/** Report a local continuous-conversation session lifecycle event. */
esp_err_t ai_mirror_gateway_report_session(const char *session_id,
                                           const char *event_type,
                                           uint32_t turn_index);

/* Continuous PCM streaming interface (方案 C). The implementation uses the
 * existing recording WebSocket and falls back to turn upload when a stream
 * cannot be opened or finalized. */
esp_err_t ai_mirror_gateway_stream_begin(const char *session_id,
                                         uint32_t sample_rate,
                                         uint32_t turn_index);
esp_err_t ai_mirror_gateway_stream_write(const int16_t *samples,
                                         size_t sample_count);
esp_err_t ai_mirror_gateway_stream_end(void);

/** Consume a pending start-recording request received from the gateway. */
bool ai_mirror_gateway_take_record_request(void);

/**
 * @brief Consume a gateway request to resume the current hands-free session
 *        without playing audio.
 */
bool ai_mirror_gateway_take_continue_listening(void);

/** Consume a gateway request to end the current long conversation. */
bool ai_mirror_gateway_take_end_conversation(void);

/** Report an I2S playback lifecycle event back to the gateway. */
esp_err_t ai_mirror_gateway_report_playback(const char *command_id,
                                            const char *event_type,
                                            uint32_t playback_ms,
                                            const char *reason);

#ifdef __cplusplus
}
#endif
