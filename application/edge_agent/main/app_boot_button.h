/*
 * SPDX-FileCopyrightText: 2026 RockBase IoT (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_boot_button_press_cb_t)(void *user_ctx);

/**
 * Configure GPIO28 (NM-CYD-C5 BOOT key) as a debounced input. The press
 * callback is invoked from a dedicated low-priority task; no ISR context.
 */
esp_err_t app_boot_button_init(int gpio_num,
                               app_boot_button_press_cb_t cb,
                               void *user_ctx);

#ifdef __cplusplus
}
#endif
