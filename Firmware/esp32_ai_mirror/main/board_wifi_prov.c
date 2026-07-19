#include "board_wifi_prov.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "wifi_provisioning/scheme_softap.h"

static const char *TAG = "wifi_prov";
static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_EVENT = BIT0;
static const int PROV_STOPPED_EVENT = BIT1;

static bool s_common_inited;
static bool s_handlers_registered;
static bool s_wifi_inited;
static bool s_wifi_started;
static bool s_sta_netif_created;
static bool s_ap_netif_created;
static bool s_prov_mgr_inited;
static bool s_prov_active;

static char s_prov_qr_payload[160];
static char s_prov_service_name[16];
static board_wifi_prov_transport_t s_prov_transport = BOARD_WIFI_PROV_TRANSPORT_SOFTAP;

#define PROV_QR_VERSION "v1"
#define PROV_TRANSPORT_BLE "ble"
#define PROV_TRANSPORT_SOFTAP "softap"
#define PROV_POP "abcd1234"
#define PROV_SERVICE_PREFIX "PROV_"
#define QRCODE_BASE_URL "https://espressif.github.io/esp-jumpstart/qrcode.html"

static void get_device_service_name(char *service_name, size_t max_len)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(service_name, max_len, PROV_SERVICE_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

const char *board_wifi_prov_transport_name(board_wifi_prov_transport_t transport)
{
    return transport == BOARD_WIFI_PROV_TRANSPORT_SOFTAP ? PROV_TRANSPORT_SOFTAP : PROV_TRANSPORT_BLE;
}

static void build_prov_payload(const char *name, const char *pop, board_wifi_prov_transport_t transport)
{
    snprintf(s_prov_qr_payload, sizeof(s_prov_qr_payload),
             "{\"ver\":\"%s\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"}",
             PROV_QR_VERSION, name, pop, board_wifi_prov_transport_name(transport));
}

static void print_prov_payload(const char *name, const char *pop, board_wifi_prov_transport_t transport)
{
    build_prov_payload(name, pop, transport);

    ESP_LOGI(TAG, "%s provisioning started", transport == BOARD_WIFI_PROV_TRANSPORT_SOFTAP ? "SoftAP" : "BLE");
    ESP_LOGI(TAG, "Provisioning app: ESP BLE Provisioning / Espressif Provisioning");
    ESP_LOGI(TAG, "Scan the QR code shown on the display, or scan/create QR from the payload below.");
    if (transport == BOARD_WIFI_PROV_TRANSPORT_SOFTAP) {
        ESP_LOGI(TAG, "SoftAP SSID: %s", name);
        ESP_LOGI(TAG, "SoftAP password: <open>");
    } else {
        ESP_LOGI(TAG, "BLE device name: %s", name);
    }
    ESP_LOGI(TAG, "Proof of Possession: %s", pop);
    ESP_LOGI(TAG, "QR payload: %s", s_prov_qr_payload);
    ESP_LOGI(TAG, "QR helper URL: %s?data=%s", QRCODE_BASE_URL, s_prov_qr_payload);
}

const char *board_wifi_prov_get_qr_payload(void)
{
    return s_prov_qr_payload;
}

const char *board_wifi_prov_get_service_name(void)
{
    return s_prov_service_name;
}

board_wifi_prov_transport_t board_wifi_prov_get_transport(void)
{
    return s_prov_transport;
}

static void prov_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning service started");
            break;
        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received Wi-Fi credentials: SSID=%s, password=%s",
                     (const char *)wifi_sta_cfg->ssid,
                     (const char *)wifi_sta_cfg->password);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG, "Provisioning failed: %s",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi auth error" : "AP not found");
            wifi_prov_mgr_reset_sm_state_on_failure();
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            break;
        case WIFI_PROV_END:
            ESP_LOGI(TAG, "Provisioning ended, releasing provisioning manager");
            wifi_prov_mgr_deinit();
            s_prov_mgr_inited = false;
            s_prov_active = false;
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, PROV_STOPPED_EVENT);
            }
            break;
        default:
            break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi STA started, connecting...");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
            if (s_wifi_event_group) {
                xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_EVENT);
            }
            esp_wifi_connect();
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "SoftAP client connected");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "SoftAP client disconnected");
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_EVENT);
        }
    } else if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        switch (event_id) {
        case PROTOCOMM_TRANSPORT_BLE_CONNECTED:
            ESP_LOGI(TAG, "BLE client connected");
            break;
        case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED:
            ESP_LOGI(TAG, "BLE client disconnected");
            break;
        default:
            break;
        }
    } else if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        switch (event_id) {
        case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
            ESP_LOGI(TAG, "Secure provisioning session established");
            break;
        case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
            ESP_LOGE(TAG, "Invalid provisioning security parameters");
            break;
        case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
            ESP_LOGE(TAG, "Provisioning PoP mismatch");
            break;
        default:
            break;
        }
    }
}

static esp_err_t init_common_services(void)
{
    if (!s_common_inited) {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs failed");
            ret = nvs_flash_init();
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "init nvs failed");

        ret = esp_netif_init();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(ret, TAG, "init netif failed");
        }

        ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(ret, TAG, "create event loop failed");
        }

        if (!s_wifi_event_group) {
            s_wifi_event_group = xEventGroupCreate();
            ESP_RETURN_ON_FALSE(s_wifi_event_group, ESP_ERR_NO_MEM, TAG, "create wifi event group failed");
        }

        s_common_inited = true;
    }

    if (!s_handlers_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL), TAG, "register provisioning event failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL), TAG, "register BLE event failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL), TAG, "register security event failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL), TAG, "register Wi-Fi event failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &prov_event_handler, NULL), TAG, "register IP event failed");
        s_handlers_registered = true;
    }

    return ESP_OK;
}

static esp_err_t init_wifi_driver(void)
{
    ESP_RETURN_ON_ERROR(init_common_services(), TAG, "init common services failed");

    if (!s_sta_netif_created) {
        esp_netif_create_default_wifi_sta();
        s_sta_netif_created = true;
    }

    if (!s_wifi_inited) {
        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "init wifi failed");
        s_wifi_inited = true;
    }

    return ESP_OK;
}

esp_err_t board_wifi_prov_prepare(void)
{
    return init_wifi_driver();
}

esp_err_t board_wifi_prov_has_saved_credentials(bool *has_credentials)
{
    ESP_RETURN_ON_FALSE(has_credentials, ESP_ERR_INVALID_ARG, TAG, "has_credentials is NULL");
    *has_credentials = false;

    ESP_RETURN_ON_ERROR(init_wifi_driver(), TAG, "init wifi for credential check failed");

    wifi_config_t wifi_cfg = {0};
    esp_err_t ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
    if (ret == ESP_ERR_WIFI_NOT_INIT) {
        return ret;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "get saved Wi-Fi config failed");

    *has_credentials = wifi_cfg.sta.ssid[0] != '\0';
    ESP_LOGI(TAG, "saved Wi-Fi credentials: %s", *has_credentials ? "yes" : "no");
    return ESP_OK;
}

bool board_wifi_prov_wait_connected(uint32_t timeout_ms)
{
    if (!s_wifi_event_group) {
        return false;
    }

    TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_EVENT,
                                           pdFALSE, pdTRUE, ticks);
    return (bits & WIFI_CONNECTED_EVENT) != 0;
}

esp_err_t board_wifi_prov_connect_saved(uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(init_wifi_driver(), TAG, "init wifi for saved connect failed");

    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_EVENT);
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi failed");
        s_wifi_started = true;
    } else {
        esp_wifi_connect();
    }

    return board_wifi_prov_wait_connected(timeout_ms) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t board_wifi_prov_start(board_wifi_prov_transport_t transport, bool reset_saved_credentials)
{
    ESP_RETURN_ON_ERROR(init_wifi_driver(), TAG, "init wifi for provisioning failed");
    ESP_LOGI(TAG, "heap before provisioning: internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (transport == BOARD_WIFI_PROV_TRANSPORT_SOFTAP && !s_ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_created = true;
    }

    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_EVENT | PROV_STOPPED_EVENT);
    }

    wifi_prov_mgr_config_t softap_cfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    wifi_prov_mgr_config_t ble_cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    wifi_prov_mgr_config_t prov_cfg = transport == BOARD_WIFI_PROV_TRANSPORT_SOFTAP ? softap_cfg : ble_cfg;

    if (!s_prov_mgr_inited) {
        ESP_RETURN_ON_ERROR(wifi_prov_mgr_init(prov_cfg), TAG, "init provisioning manager failed");
        s_prov_mgr_inited = true;
    }

    if (reset_saved_credentials) {
        wifi_prov_mgr_reset_provisioning();
    }

    if (transport == BOARD_WIFI_PROV_TRANSPORT_BLE) {
        uint8_t custom_service_uuid[] = {
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
        };
        wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);
    }

    get_device_service_name(s_prov_service_name, sizeof(s_prov_service_name));
    s_prov_transport = transport;
    build_prov_payload(s_prov_service_name, PROV_POP, s_prov_transport);

    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    wifi_prov_security1_params_t *sec_params = (wifi_prov_security1_params_t *)PROV_POP;
    const char *service_key = NULL;

    esp_err_t ret = wifi_prov_mgr_start_provisioning(security, (const void *)sec_params,
                                                     s_prov_service_name, service_key);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start provisioning failed: %s", esp_err_to_name(ret));
        wifi_prov_mgr_deinit();
        s_prov_mgr_inited = false;
        s_prov_active = false;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, PROV_STOPPED_EVENT);
        }
        return ret;
    }

    s_prov_active = true;
    s_wifi_started = true;
    ESP_LOGI(TAG, "heap after provisioning start: internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    print_prov_payload(s_prov_service_name, PROV_POP, s_prov_transport);
    return ESP_OK;
}


esp_err_t board_wifi_prov_stop(uint32_t timeout_ms)
{
    if (!s_prov_mgr_inited && !s_prov_active) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping provisioning service by button request");
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, PROV_STOPPED_EVENT);
    }
    wifi_prov_mgr_stop_provisioning();

    if (s_wifi_event_group) {
        TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, PROV_STOPPED_EVENT,
                                               pdFALSE, pdTRUE, ticks);
        if ((bits & PROV_STOPPED_EVENT) == 0) {
            ESP_LOGW(TAG, "Timed out waiting for provisioning service to stop");
            return ESP_ERR_TIMEOUT;
        }
    }

    return ESP_OK;
}
esp_err_t board_wifi_prov_start_ble(bool reset_saved_credentials)
{
    return board_wifi_prov_start(BOARD_WIFI_PROV_TRANSPORT_BLE, reset_saved_credentials);
}

esp_err_t board_wifi_prov_start_softap(bool reset_saved_credentials)
{
    return board_wifi_prov_start(BOARD_WIFI_PROV_TRANSPORT_SOFTAP, reset_saved_credentials);
}