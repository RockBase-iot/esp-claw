/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_manager_state_cb_t)(bool connected, void *user_ctx);

typedef struct {
    const char *sta_ssid;
    const char *sta_password;
    const char *ap_ssid_prefix;
    const char *ap_ssid;
    const char *ap_password;
    const char *ap_behavior;
    uint8_t ap_channel;
    uint8_t ap_max_conn;
    uint32_t max_retry;
} wifi_manager_config_t;

typedef struct {
    bool sta_connected;
    bool ap_active;
    bool sta_configured;
    const char *sta_ip;
    const char *ap_ip;
    const char *ap_ssid;
    const char *mode;
} wifi_manager_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t primary;
    wifi_auth_mode_t authmode;
} wifi_manager_scan_record_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(const wifi_manager_config_t *config);
esp_err_t wifi_manager_apply_sta_config(const wifi_manager_config_t *config);

/**
 * Bring the local soft-AP online on demand (e.g. from a long-press of the BOOT
 * key) even when it was previously closed by the "close_on_sta" behavior. Uses
 * APSTA mode when a station is configured so an active STA link is preserved.
 * Also forces the in-RAM ap_behavior to "keep" (runtime only, not persisted)
 * so the AP is not auto-closed again when STA (re)connects. Returns
 * ESP_ERR_INVALID_STATE if Wi-Fi is not started.
 */
esp_err_t wifi_manager_enable_ap(void);

/**
 * Bring the local soft-AP down on demand (e.g. from a long-press of the BOOT
 * key when the AP is already up). Switches to STA-only mode. Note that closing
 * the AP while the station is not connected can leave the device unreachable
 * until the AP is re-enabled. No-op when the AP is already down. Returns
 * ESP_ERR_INVALID_STATE if Wi-Fi is not started.
 */
esp_err_t wifi_manager_disable_ap(void);

esp_err_t wifi_manager_validate_config(const wifi_manager_config_t *config);
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);
esp_err_t wifi_manager_register_state_callback(wifi_manager_state_cb_t cb, void *user_ctx);
void wifi_manager_get_status(wifi_manager_status_t *status);
esp_netif_t *wifi_manager_get_ap_netif(void);
esp_err_t wifi_manager_scan_aps(wifi_manager_scan_record_t *records,
                                uint16_t max_records,
                                uint16_t *out_count);

#ifdef __cplusplus
}
#endif
