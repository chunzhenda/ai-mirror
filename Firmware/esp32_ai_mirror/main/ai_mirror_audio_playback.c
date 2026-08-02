#include "ai_mirror_audio_playback.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"

/* Samples live in PSRAM; queues only store ownership descriptors. */
#define PLAYBACK_QUEUE_DEPTH 8
#define STREAM_QUEUE_DEPTH 128
#define STREAM_CHUNK_SAMPLES 2048

static QueueHandle_t s_playback_queue;
static QueueHandle_t s_stream_queue;
static volatile bool s_stream_active;
static volatile bool s_stream_end;
static volatile bool s_stream_error;
static size_t s_stream_expected_samples;
static size_t s_stream_written_samples;
static bool s_stream_has_pending_byte;
static uint8_t s_stream_pending_byte;

static void stream_clear_queue(void)
{
    if (!s_stream_queue) {
        return;
    }
    ai_mirror_playback_chunk_t chunk;
    while (xQueueReceive(s_stream_queue, &chunk, 0) == pdPASS) {
        heap_caps_free(chunk.samples);
    }
}

esp_err_t ai_mirror_audio_playback_init(void)
{
    if (s_playback_queue && s_stream_queue) {
        return ESP_OK;
    }
    s_playback_queue = xQueueCreateWithCaps(
        PLAYBACK_QUEUE_DEPTH,
        sizeof(ai_mirror_playback_request_t),
        MALLOC_CAP_SPIRAM);
    s_stream_queue = xQueueCreateWithCaps(
        STREAM_QUEUE_DEPTH,
        sizeof(ai_mirror_playback_chunk_t),
        MALLOC_CAP_SPIRAM);
    if (!s_playback_queue || !s_stream_queue) {
        if (s_playback_queue) {
            vQueueDelete(s_playback_queue);
            s_playback_queue = NULL;
        }
        if (s_stream_queue) {
            vQueueDelete(s_stream_queue);
            s_stream_queue = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ai_mirror_audio_playback_submit(ai_mirror_playback_request_t *request)
{
    if (!request || (!request->streaming && (!request->samples || request->sample_count == 0))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_playback_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_playback_queue, request, 0) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    request->samples = NULL;
    request->sample_count = 0;
    return ESP_OK;
}

bool ai_mirror_audio_playback_take(ai_mirror_playback_request_t *request)
{
    return s_playback_queue && request &&
           xQueueReceive(s_playback_queue, request, 0) == pdPASS;
}

void ai_mirror_audio_playback_release(ai_mirror_playback_request_t *request)
{
    if (!request) {
        return;
    }
    if (request->samples) {
        heap_caps_free(request->samples);
    }
    if (request->streaming) {
        stream_clear_queue();
        s_stream_active = false;
        s_stream_end = false;
        s_stream_error = false;
    }
    memset(request, 0, sizeof(*request));
}

esp_err_t ai_mirror_audio_playback_begin_stream(const char *command_id,
                                                const char *turn_id,
                                                uint32_t sample_rate,
                                                size_t expected_sample_count)
{
    if (!s_playback_queue || !s_stream_queue || !command_id || !turn_id ||
        !command_id[0] || !turn_id[0] || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    stream_clear_queue();
    s_stream_active = true;
    s_stream_end = false;
    s_stream_error = false;
    s_stream_expected_samples = expected_sample_count;
    s_stream_written_samples = 0;
    s_stream_has_pending_byte = false;
    s_stream_pending_byte = 0;

    ai_mirror_playback_request_t request = {
        .samples = NULL,
        .sample_count = 0,
        .sample_rate = sample_rate,
        .expected_sample_count = expected_sample_count,
        .streaming = true,
    };
    strlcpy(request.command_id, command_id, sizeof(request.command_id));
    strlcpy(request.turn_id, turn_id, sizeof(request.turn_id));
    esp_err_t err = ai_mirror_audio_playback_submit(&request);
    if (err != ESP_OK) {
        s_stream_active = false;
        return err;
    }
    return ESP_OK;
}

esp_err_t ai_mirror_audio_playback_stream_write_bytes(const uint8_t *data,
                                                       size_t length)
{
    if (!s_stream_active || s_stream_end || s_stream_error || !data || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t offset = 0;
    if (s_stream_has_pending_byte) {
        if (length == 0) {
            return ESP_OK;
        }
        int16_t first = (int16_t)((uint16_t)s_stream_pending_byte |
                                  ((uint16_t)data[0] << 8));
        s_stream_has_pending_byte = false;
        offset = 1;
        ai_mirror_playback_chunk_t chunk = {
            .samples = heap_caps_malloc(sizeof(first), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
            .sample_count = 1,
        };
        if (!chunk.samples) {
            s_stream_error = true;
            return ESP_ERR_NO_MEM;
        }
        chunk.samples[0] = first;
        if (s_stream_expected_samples && s_stream_written_samples >= s_stream_expected_samples) {
            heap_caps_free(chunk.samples);
            s_stream_error = true;
            return ESP_ERR_INVALID_SIZE;
        }
        if (xQueueSend(s_stream_queue, &chunk, pdMS_TO_TICKS(2000)) != pdPASS) {
            heap_caps_free(chunk.samples);
            s_stream_error = true;
            return ESP_ERR_TIMEOUT;
        }
        s_stream_written_samples++;
    }

    size_t available_bytes = length - offset;
    size_t complete_samples = available_bytes / sizeof(int16_t);
    if (s_stream_expected_samples &&
        s_stream_written_samples + complete_samples > s_stream_expected_samples) {
        s_stream_error = true;
        return ESP_ERR_INVALID_SIZE;
    }
    while (complete_samples > 0) {
        size_t count = complete_samples > STREAM_CHUNK_SAMPLES
                           ? STREAM_CHUNK_SAMPLES
                           : complete_samples;
        int16_t *samples = heap_caps_malloc(
            count * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!samples) {
            s_stream_error = true;
            return ESP_ERR_NO_MEM;
        }
        memcpy(samples, data + offset, count * sizeof(int16_t));
        ai_mirror_playback_chunk_t chunk = {
            .samples = samples,
            .sample_count = count,
        };
        if (xQueueSend(s_stream_queue, &chunk, pdMS_TO_TICKS(2000)) != pdPASS) {
            heap_caps_free(samples);
            s_stream_error = true;
            return ESP_ERR_TIMEOUT;
        }
        offset += count * sizeof(int16_t);
        complete_samples -= count;
        s_stream_written_samples += count;
    }

    if (offset < length) {
        s_stream_pending_byte = data[offset];
        s_stream_has_pending_byte = true;
    }
    return ESP_OK;
}

esp_err_t ai_mirror_audio_playback_stream_end(void)
{
    if (!s_stream_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_stream_has_pending_byte ||
        (s_stream_expected_samples && s_stream_written_samples != s_stream_expected_samples)) {
        s_stream_error = true;
        s_stream_end = true;
        return ESP_ERR_INVALID_SIZE;
    }
    s_stream_end = true;
    return ESP_OK;
}

void ai_mirror_audio_playback_stream_abort(void)
{
    /*
     * Aborting a stream is terminal for the playback consumer.  Keep the
     * error flag asserted until ai_mirror_audio_playback_release() resets it;
     * otherwise the playback task sees an empty queue with neither an end nor
     * an error signal and waits forever after a gateway disconnect.
     *
     * Do not drain the queue here.  This function can run from the ESP-IDF
     * WebSocket event task while the AEC playback task is taking/releasing
     * chunks.  Draining and freeing chunks from both tasks creates a
     * use-after-free/double-free race exactly when the gateway disconnects.
     * The playback owner observes s_stream_error, exits, and
     * ai_mirror_audio_playback_release() performs the queue cleanup on that
     * owner task.
     */
    s_stream_active = false;
    s_stream_end = false;
    s_stream_error = true;
    s_stream_expected_samples = 0;
    s_stream_written_samples = 0;
    s_stream_has_pending_byte = false;
    s_stream_pending_byte = 0;
}

bool ai_mirror_audio_playback_stream_take_chunk(ai_mirror_playback_chunk_t *chunk)
{
    return s_stream_queue && chunk &&
           xQueueReceive(s_stream_queue, chunk, 0) == pdPASS;
}

void ai_mirror_audio_playback_stream_release_chunk(ai_mirror_playback_chunk_t *chunk)
{
    if (!chunk) {
        return;
    }
    heap_caps_free(chunk->samples);
    memset(chunk, 0, sizeof(*chunk));
}

bool ai_mirror_audio_playback_stream_end_received(void)
{
    return s_stream_end;
}

bool ai_mirror_audio_playback_stream_has_error(void)
{
    return s_stream_error;
}
