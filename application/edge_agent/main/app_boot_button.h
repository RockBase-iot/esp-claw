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

/**
 * Register a long-press callback fired once when the BOOT key is held for at
 * least @p duration_ms. While a long press is in effect the short-press
 * callback passed to app_boot_button_init() is suppressed for that hold (it
 * now fires on release rather than on the initial press). Pass cb = NULL to
 * disable long-press detection. Must be called after app_boot_button_init().
 */
esp_err_t app_boot_button_set_long_press(uint32_t duration_ms,
                                         app_boot_button_press_cb_t cb,
                                         void *user_ctx);

#ifdef __cplusplus
}
#endif
