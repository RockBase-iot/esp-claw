/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "basic_demo_settings.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the status screen subsystem.
 *
 * Acquires the LCD panel handle from board_manager and allocates the
 * full-frame RGB565 buffer used to render the boot splash and the
 * runtime status page.
 *
 * Must be called after esp_board_manager_init() succeeds and before the
 * first call to app_status_screen_show_splash() / show_status().
 */
esp_err_t app_status_screen_init(void);

/**
 * @brief Cache the latest settings snapshot used by the status page.
 *
 * The status page displays Wi-Fi / LLM / IM information sourced from the
 * latest cached settings. Safe to call before init.
 */
void app_status_screen_set_settings(const basic_demo_settings_t *settings);

/**
 * @brief Render the boot splash directly onto the LCD and hold for hold_ms.
 *
 * Called synchronously between board_manager init and the emote start so
 * the splash is visible before emote takes over the display.
 */
esp_err_t app_status_screen_show_splash(uint32_t hold_ms);

/**
 * @brief Schedule a status page overlay on the LCD for hold_ms.
 *
 * Spawns a short-lived task that acquires DISPLAY_ARBITER_OWNER_LUA,
 * paints the status page, holds it for the requested duration, and then
 * releases ownership back to the emote face. Safe to invoke from event
 * callbacks (non-blocking).
 */
esp_err_t app_status_screen_request_status(uint32_t hold_ms);

#ifdef __cplusplus
}
#endif
