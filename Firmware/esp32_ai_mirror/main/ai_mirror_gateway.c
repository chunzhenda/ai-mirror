#include "ai_mirror_gateway.h"
#include "ai_mirror_audio_playback.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "ai_gateway";

#if CONFIG_AI_MIRROR_GATEWAY_ENABLE

#define GATEWAY_UPLOAD_QUEUE_DEPTH 2
#define GATEWAY_HTTP_RESPONSE_BYTES 512
#define GATEWAY_WS_RESPONSE_BYTES 768
#define GATEWAY_WS_CONNECTED_BIT BIT0
#define GATEWAY_WS_ACK_BIT BIT1
#define GATEWAY_WS_ERROR_BIT BIT2
#define GATEWAY_WS_STREAM_ACK_BIT BIT3
#define GATEWAY_WS_STREAM_ERROR_BIT BIT4
#define GATEWAY_COMMAND_WS_CONNECTED_BIT BIT0
#define GATEWAY_MAX_PLAYBACK_BYTES (2 * 1024 * 1024)

typedef struct {
    int16_t *samples;
    size_t sample_count;
    uint32_t sample_rate;
    char session_id[40];
    uint32_t turn_index;
} gateway_recording_t;

typedef struct {
    char data[GATEWAY_HTTP_RESPONSE_BYTES];
    size_t length;
} gateway_http_response_t;

static QueueHandle_t s_upload_queue;
static TaskHandle_t s_gateway_task;
static volatile bool s_record_requested;
static volatile bool s_continue_listening_requested;
static volatile bool s_end_conversation_requested;
static EventGroupHandle_t s_websocket_events;
static esp_websocket_client_handle_t s_websocket_client;
static char s_websocket_url[384];
static char s_websocket_headers[160];
static char s_websocket_response[GATEWAY_WS_RESPONSE_BYTES];
static size_t s_websocket_response_length;
static volatile bool s_stream_session_active;
static EventGroupHandle_t s_command_ws_events;
static esp_websocket_client_handle_t s_command_ws_client;
static char s_command_ws_url[384];
static char s_command_ws_headers[160];
static char s_command_ws_response[GATEWAY_WS_RESPONSE_BYTES];
static size_t s_command_ws_response_length;
static volatile bool s_command_stream_active;
static char s_command_stream_command_id[AI_MIRROR_PLAYBACK_ID_BYTES];
static char s_command_stream_turn_id[AI_MIRROR_PLAYBACK_ID_BYTES];

static void gateway_handle_command(const char *response);
static bool gateway_json_string(const char *json, const char *key,
                                char *output, size_t output_size);
static esp_err_t gateway_send_event(const char *command_id,
                                    const char *event_type,
                                    const char *details_json);

static void gateway_build_url(char *output, size_t output_size, const char *path)
{
    const char *base = CONFIG_AI_MIRROR_GATEWAY_URL;
    size_t base_len = strlen(base);
    const char *separator = (base_len > 0 && base[base_len - 1] == '/') ? "" : "/";
    snprintf(output, output_size, "%s%s%s", base, separator, path);
}

static void gateway_build_websocket_url(char *output, size_t output_size)
{
    const char *base = CONFIG_AI_MIRROR_GATEWAY_URL;
    const char *rest = base;
    const char *scheme = "ws://";
    if (strncmp(base, "http://", 7) == 0) {
        rest = base + 7;
    } else if (strncmp(base, "https://", 8) == 0) {
        scheme = "wss://";
        rest = base + 8;
    }

    size_t base_len = strlen(rest);
    const char *separator = (base_len > 0 && rest[base_len - 1] == '/') ? "" : "/";
    snprintf(output, output_size, "%s%s%sws/devices/%s/recordings",
             scheme, rest, separator, CONFIG_AI_MIRROR_GATEWAY_DEVICE_ID);
}

static void gateway_build_command_websocket_url(char *output, size_t output_size)
{
    const char *base = CONFIG_AI_MIRROR_GATEWAY_URL;
    const char *rest = base;
    const char *scheme = "ws://";
    if (strncmp(base, "http://", 7) == 0) {
        rest = base + 7;
    } else if (strncmp(base, "https://", 8) == 0) {
        scheme = "wss://";
        rest = base + 8;
    }

    size_t base_len = strlen(rest);
    const char *separator = (base_len > 0 && rest[base_len - 1] == '/') ? "" : "/";
    snprintf(output, output_size, "%s%s%sws/devices/%s/commands",
             scheme, rest, separator, CONFIG_AI_MIRROR_GATEWAY_DEVICE_ID);
}

static esp_err_t gateway_http_event(esp_http_client_event_t *event)
{
    gateway_http_response_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }

    size_t remaining = sizeof(response->data) - 1 - response->length;
    size_t copy_len = (size_t)event->data_len < remaining ? (size_t)event->data_len : remaining;
    if (copy_len > 0) {
        memcpy(response->data + response->length, event->data, copy_len);
        response->length += copy_len;
        response->data[response->length] = '\0';
    }
    return ESP_OK;
}

static void gateway_websocket_event(void *handler_args,
                                    esp_event_base_t event_base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)handler_args;
    (void)event_base;
    esp_websocket_event_data_t *data = event_data;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        xEventGroupClearBits(s_websocket_events, GATEWAY_WS_ERROR_BIT);
        xEventGroupSetBits(s_websocket_events, GATEWAY_WS_CONNECTED_BIT);
        ESP_LOGI(TAG, "recording WebSocket connected: %s", s_websocket_url);
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED ||
               event_id == WEBSOCKET_EVENT_CLOSED) {
        xEventGroupClearBits(s_websocket_events, GATEWAY_WS_CONNECTED_BIT);
        s_stream_session_active = false;
        ESP_LOGW(TAG, "recording WebSocket disconnected");
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        xEventGroupSetBits(s_websocket_events, GATEWAY_WS_ERROR_BIT);
        ESP_LOGW(TAG, "recording WebSocket error");
    } else if (event_id == WEBSOCKET_EVENT_DATA && data && data->data_ptr && data->data_len > 0) {
        if (data->payload_offset == 0) {
            s_websocket_response_length = 0;
        }
        size_t remaining = sizeof(s_websocket_response) - 1 - s_websocket_response_length;
        size_t copy_len = (size_t)data->data_len < remaining ? (size_t)data->data_len : remaining;
        if (copy_len > 0) {
            memcpy(s_websocket_response + s_websocket_response_length, data->data_ptr, copy_len);
            s_websocket_response_length += copy_len;
            s_websocket_response[s_websocket_response_length] = '\0';
        }

        if (data->fin && data->payload_offset + data->data_len >= data->payload_len) {
            if (strstr(s_websocket_response, "stream_session_started") ||
                strstr(s_websocket_response, "stream_session_ended")) {
                xEventGroupSetBits(s_websocket_events, GATEWAY_WS_STREAM_ACK_BIT);
            } else if (strstr(s_websocket_response, "stream_error") ||
                       strstr(s_websocket_response, "recording_error")) {
                xEventGroupSetBits(s_websocket_events, GATEWAY_WS_STREAM_ERROR_BIT);
            } else if (strstr(s_websocket_response, "recording_saved")) {
                xEventGroupSetBits(s_websocket_events, GATEWAY_WS_ACK_BIT);
            }
        }
    }
}

static void gateway_command_websocket_event(void *handler_args,
                                            esp_event_base_t event_base,
                                            int32_t event_id,
                                            void *event_data)
{
    (void)handler_args;
    (void)event_base;
    esp_websocket_event_data_t *data = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        xEventGroupSetBits(s_command_ws_events, GATEWAY_COMMAND_WS_CONNECTED_BIT);
        ESP_LOGI(TAG, "command WebSocket connected: %s", s_command_ws_url);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        xEventGroupClearBits(s_command_ws_events, GATEWAY_COMMAND_WS_CONNECTED_BIT);
        if (s_command_stream_active) {
            ai_mirror_audio_playback_stream_abort();
            s_command_stream_active = false;
        }
        ESP_LOGW(TAG, "command WebSocket disconnected");
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        if (data) {
            ESP_LOGW(TAG,
                     "command WebSocket error: type=%d esp_err=%s errno=%d http=%d",
                     (int)data->error_handle.error_type,
                     esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
                     data->error_handle.esp_transport_sock_errno,
                     data->error_handle.esp_ws_handshake_status_code);
        } else {
            ESP_LOGW(TAG, "command WebSocket error");
        }
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || !data || !data->data_ptr || data->data_len <= 0) {
        return;
    }

    uint8_t opcode = (uint8_t)(data->op_code & 0x0F);
    /* Do not log every binary PCM frame at INFO.  A long response contains
     * hundreds of frames; formatting and UART output from the WebSocket event
     * task can starve the AEC playback task and make the bounded PCM queue
     * appear to stop draining.  Keep control-frame logs, while binary frame
     * failures are still logged below. */
    if (data->payload_offset == 0 && opcode != 2) {
        ESP_LOGI(TAG, "command WebSocket RX: opcode=0x%02x fin=%d len=%d payload=%d",
                 (unsigned)opcode, data->fin ? 1 : 0, data->data_len,
                 data->payload_len);
    }
    if ((opcode == 2 || opcode == 0) && s_command_stream_active) { /* Binary PCM frame/continuation. */
        if (!s_command_stream_active ||
            ai_mirror_audio_playback_stream_write_bytes(
                (const uint8_t *)data->data_ptr, (size_t)data->data_len) != ESP_OK) {
            ESP_LOGW(TAG, "command WebSocket PCM frame rejected");
            s_command_stream_active = false;
        }
        return;
    }
    if (opcode != 1) { /* Text control/continuation frames are handled by the client. */
        return;
    }
    if (data->payload_offset == 0) {
        s_command_ws_response_length = 0;
    }
    size_t remaining = sizeof(s_command_ws_response) - 1 - s_command_ws_response_length;
    size_t copy_len = (size_t)data->data_len < remaining
                          ? (size_t)data->data_len
                          : remaining;
    if (copy_len > 0) {
        memcpy(s_command_ws_response + s_command_ws_response_length,
               data->data_ptr, copy_len);
        s_command_ws_response_length += copy_len;
        s_command_ws_response[s_command_ws_response_length] = '\0';
    }
    if (!data->fin || data->payload_offset + data->data_len < data->payload_len) {
        return;
    }

    char message_type[48];
    if (gateway_json_string(s_command_ws_response, "type", message_type, sizeof(message_type)) &&
        strcmp(message_type, "audio_stream_end") == 0) {
        if (strstr(s_command_ws_response, "\"error\"")) {
            ai_mirror_audio_playback_stream_abort();
            s_command_stream_active = false;
            gateway_send_event(s_command_stream_command_id, "playback_failed",
                               "\"reason\":\"gateway audio stream error\"");
        } else {
            esp_err_t err = ai_mirror_audio_playback_stream_end();
            s_command_stream_active = false;
            if (err != ESP_OK) {
                gateway_send_event(s_command_stream_command_id, "playback_failed",
                                   "\"reason\":\"invalid streamed PCM\"");
            } else {
                gateway_send_event(s_command_stream_command_id, "playback_buffered",
                                   "\"transport\":\"websocket_pcm\"");
            }
        }
        return;
    }
    gateway_handle_command(s_command_ws_response);
}

static bool gateway_json_string(const char *json, const char *key,
                                char *output, size_t output_size)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *cursor = strstr(json, needle);
    if (!cursor) {
        return false;
    }
    cursor = strchr(cursor + strlen(needle), ':');
    if (!cursor) {
        return false;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor != '"') {
        return false;
    }
    cursor++;
    const char *end = strchr(cursor, '"');
    if (!end) {
        return false;
    }
    size_t length = (size_t)(end - cursor);
    if (length >= output_size) {
        length = output_size - 1;
    }
    memcpy(output, cursor, length);
    output[length] = '\0';
    return true;
}

static bool gateway_json_uint(const char *json, const char *key, uint32_t *output)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *cursor = strstr(json, needle);
    if (!cursor) {
        return false;
    }
    cursor = strchr(cursor + strlen(needle), ':');
    if (!cursor) {
        return false;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    char *end = NULL;
    unsigned long value = strtoul(cursor, &end, 10);
    if (end == cursor || value > UINT32_MAX) {
        return false;
    }
    *output = (uint32_t)value;
    return true;
}

static esp_err_t gateway_send_event(const char *command_id,
                                    const char *event_type,
                                    const char *details_json)
{
    char body[384];
    snprintf(body, sizeof(body),
             "{\"command_id\":\"%s\",\"type\":\"%s\",\"details\":{%s}}",
             command_id ? command_id : "",
             event_type,
             details_json ? details_json : "");

    bool command_ws_configured = s_command_ws_client && s_command_ws_events;
    if (command_ws_configured &&
        (xEventGroupGetBits(s_command_ws_events) & GATEWAY_COMMAND_WS_CONNECTED_BIT)) {
        int sent = esp_websocket_client_send_text(
            s_command_ws_client, body, strlen(body),
            pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
        if (sent == (int)strlen(body)) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "command WebSocket event send failed");
        return ESP_FAIL;
    }

    /*
     * When the command WebSocket exists but is disconnected, do not create a
     * second HTTP/TCP connection from the AEC playback task.  A disconnect can
     * race with lwIP socket teardown; the old fallback path occasionally
     * entered esp_http_client_perform()/close() during that window and caused a
     * LoadProhibited panic.  Events are transient (playback_failed,
     * playback_aborted, etc.); dropping one while offline is safer than
     * crashing the device.  The HTTP path remains available when the command
     * WebSocket could not be allocated at all.
     */
    if (command_ws_configured) {
        ESP_LOGW(TAG, "command WebSocket offline; event dropped: %s",
                 event_type ? event_type : "unknown");
        return ESP_ERR_INVALID_STATE;
    }

    char path[192];
    snprintf(path, sizeof(path), "api/devices/%s/events", CONFIG_AI_MIRROR_GATEWAY_DEVICE_ID);
    char url[320];
    gateway_build_url(url, sizeof(url), path);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-Token", CONFIG_AI_MIRROR_GATEWAY_TOKEN);
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }
    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

static esp_err_t gateway_download_playback(const char *audio_path,
                                           const char *command_id,
                                           const char *turn_id,
                                           uint32_t sample_rate,
                                           uint32_t sample_count)
{
    if (sample_rate != 16000 || sample_count == 0 ||
        sample_count > GATEWAY_MAX_PLAYBACK_BYTES / sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char url[384];
    gateway_build_url(url, sizeof(url), audio_path[0] == '/' ? audio_path + 1 : audio_path);
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 60000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "X-Device-Token", CONFIG_AI_MIRROR_GATEWAY_TOKEN);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    size_t expected_bytes = (size_t)sample_count * sizeof(int16_t);
    if (status != 200 || content_length != (int64_t)expected_bytes) {
        ESP_LOGW(TAG, "playback download rejected: HTTP=%d length=%lld expected=%u",
                 status, content_length, (unsigned)expected_bytes);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int16_t *samples = heap_caps_malloc(
        expected_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!samples) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    size_t received = 0;
    while (received < expected_bytes) {
        int count = esp_http_client_read(
            client, (char *)samples + received, expected_bytes - received);
        if (count <= 0) {
            err = count < 0 ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
            break;
        }
        received += (size_t)count;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || received != expected_bytes) {
        heap_caps_free(samples);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    ai_mirror_playback_request_t request = {
        .samples = samples,
        .sample_count = sample_count,
        .sample_rate = sample_rate,
    };
    strlcpy(request.command_id, command_id, sizeof(request.command_id));
    strlcpy(request.turn_id, turn_id, sizeof(request.turn_id));
    err = ai_mirror_audio_playback_submit(&request);
    if (err != ESP_OK) {
        heap_caps_free(samples);
        return err;
    }
    ESP_LOGI(TAG, "LLM playback queued: turn=%s samples=%u bytes=%u",
             turn_id, (unsigned)sample_count, (unsigned)expected_bytes);
    return ESP_OK;
}

static esp_err_t gateway_upload_recording(const gateway_recording_t *recording)
{
    if (!s_websocket_client || !s_websocket_events) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_websocket_events,
        GATEWAY_WS_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
    if (!(bits & GATEWAY_WS_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "recording upload skipped: WebSocket is not connected");
        return ESP_ERR_TIMEOUT;
    }

    char metadata[320];
    int metadata_len = snprintf(
        metadata, sizeof(metadata),
        "{\"type\":\"recording_pcm\",\"protocol_version\":1,"
        "\"transport\":\"turn\",\"session_id\":\"%s\","
        "\"turn_index\":%" PRIu32 ",\"sample_rate\":%" PRIu32
        ",\"channels\":1,\"bits_per_sample\":16,\"sample_count\":%u}",
        recording->session_id,
        recording->turn_index,
        recording->sample_rate,
        (unsigned)recording->sample_count);
    if (metadata_len <= 0 || metadata_len >= (int)sizeof(metadata)) {
        return ESP_ERR_INVALID_SIZE;
    }

    xEventGroupClearBits(s_websocket_events, GATEWAY_WS_ACK_BIT | GATEWAY_WS_ERROR_BIT);
    int sent = esp_websocket_client_send_text(
        s_websocket_client, metadata, metadata_len,
        pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
    if (sent != metadata_len) {
        ESP_LOGW(TAG, "recording metadata send failed: sent=%d expected=%d", sent, metadata_len);
        return ESP_FAIL;
    }

    size_t total_bytes = recording->sample_count * sizeof(int16_t);
    sent = esp_websocket_client_send_bin(
        s_websocket_client, (const char *)recording->samples, total_bytes,
        pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
    if (sent != (int)total_bytes) {
        ESP_LOGW(TAG, "recording PCM send failed: sent=%d expected=%u",
                 sent, (unsigned)total_bytes);
        return ESP_FAIL;
    }

    bits = xEventGroupWaitBits(
        s_websocket_events,
        GATEWAY_WS_ACK_BIT | GATEWAY_WS_ERROR_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
    if (bits & GATEWAY_WS_ACK_BIT) {
        ESP_LOGI(TAG, "recording uploaded via WebSocket: %u samples, ACK received",
                 (unsigned)recording->sample_count);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "recording upload did not receive gateway ACK");
    return (bits & GATEWAY_WS_ERROR_BIT) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

static void gateway_handle_command(const char *response)
{
    char command_id[64];
    char command_type[48];
    if (!gateway_json_string(response, "id", command_id, sizeof(command_id)) ||
        !gateway_json_string(response, "type", command_type, sizeof(command_type))) {
        ESP_LOGW(TAG, "invalid command response: %s", response);
        return;
    }

    ESP_LOGI(TAG, "command received: id=%s type=%s", command_id, command_type);
    if (strcmp(command_type, "ping") == 0) {
        gateway_send_event(command_id, "pong", "\"ok\":true");
    } else if (strcmp(command_type, "get_status") == 0) {
        char details[192];
        snprintf(details, sizeof(details),
                 "\"free_heap\":%u,\"minimum_free_heap\":%u,\"uptime_ms\":%" PRIu64,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 (uint64_t)(esp_timer_get_time() / 1000));
        gateway_send_event(command_id, "status", details);
    } else if (strcmp(command_type, "start_recording") == 0) {
        __atomic_store_n(&s_record_requested, true, __ATOMIC_RELEASE);
        gateway_send_event(command_id, "recording_requested", "\"accepted\":true");
    } else if (strcmp(command_type, "continue_listening") == 0) {
        __atomic_store_n(
            &s_continue_listening_requested, true, __ATOMIC_RELEASE);
        gateway_send_event(
            command_id, "listening_resumed", "\"accepted\":true");
    } else if (strcmp(command_type, "end_conversation") == 0) {
        __atomic_store_n(
            &s_end_conversation_requested, true, __ATOMIC_RELEASE);
        gateway_send_event(
            command_id, "conversation_end_requested", "\"accepted\":true");
    } else if (strcmp(command_type, "play_audio") == 0) {
        char audio_path[192];
        char turn_id[AI_MIRROR_PLAYBACK_ID_BYTES];
        char transport[32] = "";
        uint32_t sample_rate = 0;
        uint32_t sample_count = 0;
        if (!gateway_json_string(response, "audio_path", audio_path, sizeof(audio_path)) ||
            !gateway_json_string(response, "turn_id", turn_id, sizeof(turn_id)) ||
            !gateway_json_uint(response, "sample_rate", &sample_rate) ||
            !gateway_json_uint(response, "sample_count", &sample_count)) {
            gateway_send_event(command_id, "playback_failed",
                               "\"reason\":\"invalid command payload\"");
            return;
        }
        (void)gateway_json_string(response, "transport", transport, sizeof(transport));
        if (strcmp(transport, "websocket_pcm") == 0) {
            esp_err_t err = ai_mirror_audio_playback_begin_stream(
                command_id, turn_id, sample_rate, sample_count);
            if (err != ESP_OK) {
                char details[128];
                snprintf(details, sizeof(details),
                         "\"reason\":\"stream begin failed: %s\"",
                         esp_err_to_name(err));
                gateway_send_event(command_id, "playback_failed", details);
                return;
            }
            strlcpy(s_command_stream_command_id, command_id,
                    sizeof(s_command_stream_command_id));
            strlcpy(s_command_stream_turn_id, turn_id,
                    sizeof(s_command_stream_turn_id));
            s_command_stream_active = true;
            ESP_LOGI(TAG, "streamed playback started: turn=%s samples=%u",
                     turn_id, (unsigned)sample_count);
            return;
        }
        int64_t started_us = esp_timer_get_time();
        esp_err_t err = gateway_download_playback(
            audio_path, command_id, turn_id, sample_rate, sample_count);
        if (err == ESP_OK) {
            uint32_t download_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
            char details[96];
            snprintf(details, sizeof(details),
                     "\"sample_count\":%u,\"download_ms\":%u",
                     (unsigned)sample_count, (unsigned)download_ms);
            gateway_send_event(command_id, "playback_buffered", details);
        } else {
            char details[128];
            snprintf(details, sizeof(details),
                     "\"reason\":\"download/queue failed: %s\"", esp_err_to_name(err));
            gateway_send_event(command_id, "playback_failed", details);
        }
    } else {
        gateway_send_event(command_id, "command_rejected", "\"reason\":\"unsupported command\"");
    }
}

static esp_err_t gateway_poll_command(void)
{
    char path[224];
    snprintf(path, sizeof(path), "api/devices/%s/commands?wait_ms=%d",
             CONFIG_AI_MIRROR_GATEWAY_DEVICE_ID,
             CONFIG_AI_MIRROR_GATEWAY_POLL_MS);
    char url[352];
    gateway_build_url(url, sizeof(url), path);

    gateway_http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_AI_MIRROR_GATEWAY_POLL_MS + 1500,
        .event_handler = gateway_http_event,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "X-Device-Token", CONFIG_AI_MIRROR_GATEWAY_TOKEN);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200 && response.length > 0) {
        gateway_handle_command(response.data);
    } else if (err == ESP_OK && status != 204) {
        ESP_LOGW(TAG, "command poll returned HTTP %d", status);
    }
    return err;
}

static void gateway_task(void *argument)
{
    (void)argument;
    gateway_send_event(NULL, "online",
                       "\"command_transport\":\"websocket-or-http-poll\",\"recording_transport\":\"websocket-turn-or-stream\"");

    while (1) {
        gateway_recording_t recording;
        while (xQueueReceive(s_upload_queue, &recording, 0) == pdPASS) {
            gateway_upload_recording(&recording);
            heap_caps_free(recording.samples);
        }

        bool command_ws_connected = s_command_ws_events &&
            (xEventGroupGetBits(s_command_ws_events) & GATEWAY_COMMAND_WS_CONNECTED_BIT);
        if (!command_ws_connected) {
            esp_err_t err = gateway_poll_command();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "gateway poll failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

#endif

esp_err_t ai_mirror_gateway_start(void)
{
#if CONFIG_AI_MIRROR_GATEWAY_ENABLE
    if (s_gateway_task) {
        return ESP_OK;
    }
    s_upload_queue = xQueueCreate(GATEWAY_UPLOAD_QUEUE_DEPTH, sizeof(gateway_recording_t));
    if (!s_upload_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_websocket_events = xEventGroupCreate();
    if (!s_websocket_events) {
        vQueueDelete(s_upload_queue);
        s_upload_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    gateway_build_websocket_url(s_websocket_url, sizeof(s_websocket_url));
    snprintf(s_websocket_headers, sizeof(s_websocket_headers),
             "X-Device-Token: %s\r\n", CONFIG_AI_MIRROR_GATEWAY_TOKEN);
    const esp_websocket_client_config_t websocket_config = {
        .uri = s_websocket_url,
        .task_name = "gateway_ws",
        .task_stack = 6144,
        .buffer_size = 1024,
        .headers = s_websocket_headers,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS,
        /* The local gateway is on the same LAN.  Keep the socket alive with
         * TCP/WebSocket traffic, but do not tear down the recording stream if
         * a PONG is delayed while the Wi-Fi driver is busy with PCM. */
        .ping_interval_sec = 60,
        .disable_pingpong_discon = true,
    };
    s_websocket_client = esp_websocket_client_init(&websocket_config);
    if (!s_websocket_client) {
        vEventGroupDelete(s_websocket_events);
        s_websocket_events = NULL;
        vQueueDelete(s_upload_queue);
        s_upload_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_register_events(
        s_websocket_client, WEBSOCKET_EVENT_ANY, gateway_websocket_event, NULL);
    if (err == ESP_OK) {
        err = esp_websocket_client_start(s_websocket_client);
    }
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_websocket_client);
        s_websocket_client = NULL;
        vEventGroupDelete(s_websocket_events);
        s_websocket_events = NULL;
        vQueueDelete(s_upload_queue);
        s_upload_queue = NULL;
        return err;
    }

    /* Command channel is an optimization and has the HTTP long-poll fallback. */
    s_command_ws_events = xEventGroupCreate();
    if (s_command_ws_events) {
        gateway_build_command_websocket_url(
            s_command_ws_url, sizeof(s_command_ws_url));
        snprintf(s_command_ws_headers, sizeof(s_command_ws_headers),
                 "X-Device-Token: %s\r\n", CONFIG_AI_MIRROR_GATEWAY_TOKEN);
        const esp_websocket_client_config_t command_config = {
            .uri = s_command_ws_url,
            .task_name = "gateway_cmd_ws",
            .task_stack = 6144,
            .buffer_size = 4096,
            .headers = s_command_ws_headers,
            .reconnect_timeout_ms = 2000,
            /* Keep PCM ingest below the AEC task priority.  The event callback
             * may wait for a bounded playback queue slot; playback must keep
             * draining that queue while the network task is back-pressured. */
            .task_prio = 4,
            /* Long PCM streams can briefly occupy the event task while the
             * playback queue catches up.  Keep the persistent command socket
             * alive long enough to receive the final audio_stream_end frame. */
            .network_timeout_ms = 60000,
            .ping_interval_sec = 10,
            .disable_pingpong_discon = true,
        };
        s_command_ws_client = esp_websocket_client_init(&command_config);
        if (s_command_ws_client) {
            err = esp_websocket_register_events(
                s_command_ws_client, WEBSOCKET_EVENT_ANY,
                gateway_command_websocket_event, NULL);
            if (err == ESP_OK) {
                err = esp_websocket_client_start(s_command_ws_client);
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "command WebSocket unavailable: %s; using HTTP poll",
                         esp_err_to_name(err));
                esp_websocket_client_destroy(s_command_ws_client);
                s_command_ws_client = NULL;
            }
        } else {
            ESP_LOGW(TAG, "command WebSocket allocation failed; using HTTP poll");
        }
    } else {
        ESP_LOGW(TAG, "command WebSocket event group allocation failed; using HTTP poll");
    }

    BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        gateway_task,
        "ai_gateway",
        8192,
        NULL,
        3,
        &s_gateway_task,
        0,
        MALLOC_CAP_SPIRAM);
    if (result != pdPASS) {
        if (s_command_ws_client) {
            esp_websocket_client_stop(s_command_ws_client);
            esp_websocket_client_destroy(s_command_ws_client);
            s_command_ws_client = NULL;
        }
        if (s_command_ws_events) {
            vEventGroupDelete(s_command_ws_events);
            s_command_ws_events = NULL;
        }
        esp_websocket_client_stop(s_websocket_client);
        esp_websocket_client_destroy(s_websocket_client);
        s_websocket_client = NULL;
        vEventGroupDelete(s_websocket_events);
        s_websocket_events = NULL;
        vQueueDelete(s_upload_queue);
        s_upload_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "gateway client started: %s device=%s recording_ws=%s command_ws=%s",
             CONFIG_AI_MIRROR_GATEWAY_URL,
             CONFIG_AI_MIRROR_GATEWAY_DEVICE_ID,
             s_websocket_url,
             s_command_ws_url);
#else
    ESP_LOGI(TAG, "gateway client disabled by menuconfig");
#endif
    return ESP_OK;
}

esp_err_t ai_mirror_gateway_submit_turn_pcm(const int16_t *samples,
                                            size_t sample_count,
                                            uint32_t sample_rate,
                                            const char *session_id,
                                            uint32_t turn_index)
{
#if CONFIG_AI_MIRROR_GATEWAY_ENABLE
    if (!samples || sample_count == 0 || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_upload_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes = sample_count * sizeof(int16_t);
    int16_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, samples, bytes);

    gateway_recording_t recording = {
        .samples = copy,
        .sample_count = sample_count,
        .sample_rate = sample_rate,
        .turn_index = turn_index,
    };
    strlcpy(recording.session_id, session_id ? session_id : "",
            sizeof(recording.session_id));
    if (xQueueSend(s_upload_queue, &recording, 0) != pdPASS) {
        heap_caps_free(copy);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "recording queued for WebSocket upload: %u samples", (unsigned)sample_count);
    return ESP_OK;
#else
    (void)samples;
    (void)sample_count;
    (void)sample_rate;
    (void)session_id;
    (void)turn_index;
    return ESP_OK;
#endif
}

esp_err_t ai_mirror_gateway_submit_pcm(const int16_t *samples,
                                       size_t sample_count,
                                       uint32_t sample_rate)
{
    return ai_mirror_gateway_submit_turn_pcm(
        samples, sample_count, sample_rate, "", 0);
}

bool ai_mirror_gateway_take_record_request(void)
{
#if CONFIG_AI_MIRROR_GATEWAY_ENABLE
    return __atomic_exchange_n(&s_record_requested, false, __ATOMIC_ACQ_REL);
#else
    return false;
#endif
}

bool ai_mirror_gateway_take_continue_listening(void)
{
#if CONFIG_AI_MIRROR_GATEWAY_ENABLE
    return __atomic_exchange_n(
        &s_continue_listening_requested, false, __ATOMIC_ACQ_REL);
#else
    return false;
#endif
}

bool ai_mirror_gateway_take_end_conversation(void)
{
#if CONFIG_AI_MIRROR_GATEWAY_ENABLE
    return __atomic_exchange_n(
        &s_end_conversation_requested, false, __ATOMIC_ACQ_REL);
#else
    return false;
#endif
}

esp_err_t ai_mirror_gateway_report_playback(const char *command_id,
                                            const char *event_type,
                                            uint32_t playback_ms,
                                            const char *reason)
{
#if CONFIG_AI_MIRROR_GATEWAY_ENABLE
    if (!command_id || !event_type) {
        return ESP_ERR_INVALID_ARG;
    }
    char details[192];
    snprintf(details, sizeof(details),
             "\"playback_ms\":%u,\"reason\":\"%s\"",
             (unsigned)playback_ms, reason ? reason : "");
    return gateway_send_event(command_id, event_type, details);
#else
    (void)command_id;
    (void)event_type;
    (void)playback_ms;
    (void)reason;
    return ESP_OK;
#endif
}

esp_err_t ai_mirror_gateway_report_session(const char *session_id,
                                           const char *event_type,
                                           uint32_t turn_index)
{
#if CONFIG_AI_MIRROR_GATEWAY_ENABLE
    if (!session_id || !event_type) {
        return ESP_ERR_INVALID_ARG;
    }
    char details[160];
    snprintf(details, sizeof(details),
             "\"session_id\":\"%s\",\"turn_index\":%" PRIu32,
             session_id, turn_index);
    return gateway_send_event(NULL, event_type, details);
#else
    (void)session_id;
    (void)event_type;
    (void)turn_index;
    return ESP_OK;
#endif
}

esp_err_t ai_mirror_gateway_stream_begin(const char *session_id,
                                         uint32_t sample_rate,
                                         uint32_t turn_index)
{
#if CONFIG_AI_MIRROR_GATEWAY_ENABLE
    if (!s_websocket_client || !s_websocket_events || !session_id ||
        !session_id[0] || sample_rate == 0 || s_stream_session_active) {
        return ESP_ERR_INVALID_ARG;
    }
    EventBits_t connected = xEventGroupWaitBits(
        s_websocket_events,
        GATEWAY_WS_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
    if (!(connected & GATEWAY_WS_CONNECTED_BIT)) {
        return ESP_ERR_TIMEOUT;
    }
    char stream_id[40];
    snprintf(stream_id, sizeof(stream_id), "%s-%" PRIu32, session_id,
             (uint32_t)(esp_timer_get_time() / 1000));
    char metadata[320];
    int metadata_len = snprintf(
        metadata, sizeof(metadata),
        "{\"type\":\"stream_session_start\",\"protocol_version\":2,"
        "\"transport\":\"stream\",\"stream_id\":\"%s\","
        "\"session_id\":\"%s\",\"turn_index\":%" PRIu32 ","
        "\"sample_rate\":%" PRIu32 ",\"channels\":1,"
        "\"bits_per_sample\":16}",
        stream_id, session_id, turn_index, sample_rate);
    if (metadata_len <= 0 || metadata_len >= (int)sizeof(metadata)) {
        return ESP_ERR_INVALID_SIZE;
    }
    xEventGroupClearBits(
        s_websocket_events,
        GATEWAY_WS_STREAM_ACK_BIT | GATEWAY_WS_STREAM_ERROR_BIT);
    int sent = esp_websocket_client_send_text(
        s_websocket_client, metadata, metadata_len,
        pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
    if (sent != metadata_len) {
        return ESP_FAIL;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_websocket_events,
        GATEWAY_WS_STREAM_ACK_BIT | GATEWAY_WS_STREAM_ERROR_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
    if (bits & GATEWAY_WS_STREAM_ACK_BIT) {
        s_stream_session_active = true;
        ESP_LOGI(TAG, "stream session started: %s", stream_id);
        return ESP_OK;
    }
    return (bits & GATEWAY_WS_STREAM_ERROR_BIT) ? ESP_FAIL : ESP_ERR_TIMEOUT;
#else
    (void)session_id;
    (void)sample_rate;
    (void)turn_index;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t ai_mirror_gateway_stream_write(const int16_t *samples,
                                         size_t sample_count)
{
#if CONFIG_AI_MIRROR_GATEWAY_ENABLE
    if (!s_stream_session_active || !s_websocket_client || !samples ||
        sample_count == 0 || sample_count > (GATEWAY_MAX_PLAYBACK_BYTES / sizeof(int16_t))) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t bytes = sample_count * sizeof(int16_t);
    int sent = esp_websocket_client_send_bin(
        s_websocket_client, (const char *)samples, bytes,
        pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
    if (sent != (int)bytes) {
        ESP_LOGW(TAG, "stream PCM send failed: sent=%d expected=%u",
                 sent, (unsigned)bytes);
        return ESP_FAIL;
    }
    return ESP_OK;
#else
    (void)samples;
    (void)sample_count;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t ai_mirror_gateway_stream_end(void)
{
#if CONFIG_AI_MIRROR_GATEWAY_ENABLE
    if (!s_stream_session_active || !s_websocket_client || !s_websocket_events) {
        return ESP_ERR_INVALID_STATE;
    }
    static const char end_message[] = "{\"type\":\"stream_session_end\"}";
    xEventGroupClearBits(
        s_websocket_events,
        GATEWAY_WS_STREAM_ACK_BIT | GATEWAY_WS_STREAM_ERROR_BIT);
    int sent = esp_websocket_client_send_text(
        s_websocket_client, end_message, sizeof(end_message) - 1,
        pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
    if (sent != (int)(sizeof(end_message) - 1)) {
        s_stream_session_active = false;
        return ESP_FAIL;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_websocket_events,
        GATEWAY_WS_STREAM_ACK_BIT | GATEWAY_WS_STREAM_ERROR_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(CONFIG_AI_MIRROR_GATEWAY_HTTP_TIMEOUT_MS));
    s_stream_session_active = false;
    if (bits & GATEWAY_WS_STREAM_ACK_BIT) {
        ESP_LOGI(TAG, "stream session ended");
        return ESP_OK;
    }
    return (bits & GATEWAY_WS_STREAM_ERROR_BIT) ? ESP_FAIL : ESP_ERR_TIMEOUT;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
