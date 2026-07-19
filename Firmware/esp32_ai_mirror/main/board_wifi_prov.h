#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_WIFI_PROV_TRANSPORT_BLE = 0,
    BOARD_WIFI_PROV_TRANSPORT_SOFTAP,
} board_wifi_prov_transport_t;

esp_err_t board_wifi_prov_prepare(void);
esp_err_t board_wifi_prov_has_saved_credentials(bool *has_credentials);
esp_err_t board_wifi_prov_connect_saved(uint32_t timeout_ms);
esp_err_t board_wifi_prov_start(board_wifi_prov_transport_t transport, bool reset_saved_credentials);
esp_err_t board_wifi_prov_start_ble(bool reset_saved_credentials);
esp_err_t board_wifi_prov_start_softap(bool reset_saved_credentials);
esp_err_t board_wifi_prov_stop(uint32_t timeout_ms);
bool board_wifi_prov_wait_connected(uint32_t timeout_ms);

/* QR provisioning info for the display/app. Valid after board_wifi_prov_start(). */
const char *board_wifi_prov_get_qr_payload(void);
const char *board_wifi_prov_get_service_name(void);
board_wifi_prov_transport_t board_wifi_prov_get_transport(void);
const char *board_wifi_prov_transport_name(board_wifi_prov_transport_t transport);

#ifdef __cplusplus
}
#endif