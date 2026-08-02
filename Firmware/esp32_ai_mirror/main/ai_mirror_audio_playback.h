#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AI_MIRROR_PLAYBACK_ID_BYTES 33

typedef struct {
    int16_t *samples;
    size_t sample_count;
    uint32_t sample_rate;
    size_t expected_sample_count;
    bool streaming;
    char command_id[AI_MIRROR_PLAYBACK_ID_BYTES];
    char turn_id[AI_MIRROR_PLAYBACK_ID_BYTES];
} ai_mirror_playback_request_t;

typedef struct {
    int16_t *samples;
    size_t sample_count;
} ai_mirror_playback_chunk_t;

esp_err_t ai_mirror_audio_playback_init(void);

/** Transfer ownership of a PSRAM PCM buffer to the audio task on success. */
esp_err_t ai_mirror_audio_playback_submit(ai_mirror_playback_request_t *request);

/** Non-blocking receive used only by the I2S/AEC task. */
bool ai_mirror_audio_playback_take(ai_mirror_playback_request_t *request);

void ai_mirror_audio_playback_release(ai_mirror_playback_request_t *request);

/** Begin a PCM stream; ownership of subsequent binary chunks is transferred to the playback task. */
esp_err_t ai_mirror_audio_playback_begin_stream(const char *command_id,
                                                const char *turn_id,
                                                uint32_t sample_rate,
                                                size_t expected_sample_count);

esp_err_t ai_mirror_audio_playback_stream_write_bytes(const uint8_t *data,
                                                       size_t length);

esp_err_t ai_mirror_audio_playback_stream_end(void);

void ai_mirror_audio_playback_stream_abort(void);

bool ai_mirror_audio_playback_stream_take_chunk(ai_mirror_playback_chunk_t *chunk);

void ai_mirror_audio_playback_stream_release_chunk(ai_mirror_playback_chunk_t *chunk);

bool ai_mirror_audio_playback_stream_end_received(void);

bool ai_mirror_audio_playback_stream_has_error(void);

#ifdef __cplusplus
}
#endif
