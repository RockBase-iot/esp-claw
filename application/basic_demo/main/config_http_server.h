/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifndef BASIC_DEMO_HTTP_SERVER_PORT
#define BASIC_DEMO_HTTP_SERVER_PORT 80
#endif

esp_err_t config_http_server_init(const char *storage_base_path);
esp_err_t config_http_server_start(void);
esp_err_t config_http_server_stop(void);

/**
 * @brief  Expose an additional VFS mount under a virtual prefix in the file
 *         manager. See `http_server_register_mount` in edge_agent for
 *         semantics. `vroot` and `real_path` must start with `/`.
 */
esp_err_t config_http_server_register_mount(const char *vroot,
                                            const char *real_path,
                                            const char *label,
                                            bool (*alive_cb)(void));

/** Unregister a virtual mount registered via the function above. */
esp_err_t config_http_server_unregister_mount(const char *vroot);
