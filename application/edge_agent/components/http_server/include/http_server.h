/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool wifi_connected;
    const char *ip;
    bool ap_active;
    const char *ap_ssid;
    const char *ap_ip;
    const char *wifi_mode;
} http_server_wifi_status_t;

typedef struct {
    bool active;
    bool configured;
    bool completed;
    bool persisted;
    char session_key[64];
    char status[32];
    char message[160];
    char qr_data_url[256];
    char account_id[64];
    char user_id[96];
    char token[256];
    char base_url[160];
} http_server_wechat_login_status_t;

typedef struct {
    esp_err_t (*load_config)(app_config_t *config);
    esp_err_t (*save_config)(const app_config_t *config);
    esp_err_t (*get_wifi_status)(http_server_wifi_status_t *status);
    esp_err_t (*restart_device)(void);
    esp_err_t (*wechat_login_start)(const char *account_id, bool force);
    esp_err_t (*wechat_login_get_status)(http_server_wechat_login_status_t *status);
    esp_err_t (*wechat_login_cancel)(void);
    esp_err_t (*wechat_login_mark_persisted)(void);
} http_server_services_t;

typedef struct {
    const char *storage_base_path;
    http_server_services_t services;
} http_server_config_t;

esp_err_t http_server_init(const http_server_config_t *config);
esp_err_t http_server_start(void);
esp_err_t http_server_stop(void);
esp_err_t http_server_webim_bind_im(void);

/**
 * @brief  Register an additional virtual filesystem mount that the file
 *         manager API should expose alongside the default storage root.
 *         The default storage root is always served at virtual path `/`.
 *
 *         When a request hits `/api/files?path=<vroot>/...` the prefix is
 *         transparently rewritten to `<real_path>/...` on the underlying
 *         VFS. The mount also appears as a synthetic directory entry in the
 *         root listing (e.g. `{name:"sdcard", path:"/sdcard", virtual:"sd",
 *         is_dir:true}`).
 *
 *         Safe to call multiple times; subsequent calls with the same vroot
 *         replace the previous mapping. Up to 4 extra mounts are supported.
 *
 * @param[in]  vroot      Virtual prefix shown to clients, e.g. "/sdcard".
 *                        Must start with `/`. NULL or empty unregisters.
 * @param[in]  real_path  Backing VFS path, e.g. "/sdcard". Must start with `/`.
 * @param[in]  label      Short tag exposed to the UI (e.g. "sd"). Optional.
 */
/**
 * @brief Optional health probe for an extra mount. Returning false causes
 *        the mount to be auto-unregistered (e.g. SD card hot-removed).
 */
typedef bool (*http_server_mount_alive_cb_t)(void);

esp_err_t http_server_register_mount(const char *vroot,
                                     const char *real_path,
                                     const char *label,
                                     http_server_mount_alive_cb_t alive_cb);

/** Unregister a virtual mount previously registered via the function above. */
esp_err_t http_server_unregister_mount(const char *vroot);

#ifdef __cplusplus
}
#endif
