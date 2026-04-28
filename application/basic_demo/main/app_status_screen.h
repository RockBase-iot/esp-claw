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

typedef enum {
    APP_STATUS_PAGE_STATUS    = 0,    /* System / Wi-Fi / LLM / IM status */
    APP_STATUS_PAGE_MESSAGES  = 1,    /* Inbox list                       */
    APP_STATUS_PAGE_COUNT
} app_status_page_t;

esp_err_t app_status_screen_init(void);

void app_status_screen_set_settings(const basic_demo_settings_t *settings);

/** Render the boot splash and hold for hold_ms. Synchronous. */
esp_err_t app_status_screen_show_splash(uint32_t hold_ms);

/** Show one of the pages and hold for hold_ms (0 = until released). */
esp_err_t app_status_screen_show_page(app_status_page_t page, uint32_t hold_ms);

/** Switch to the next page (cyclic) and hold for the default duration. */
esp_err_t app_status_screen_next_page(void);

/** Repaint the active page if currently visible (e.g. inbox changed). */
void app_status_screen_request_refresh(void);

/**
 * Scroll the message list (only effective on APP_STATUS_PAGE_MESSAGES).
 * Positive `delta` scrolls down (toward older messages), negative scrolls up
 * (toward newer messages). Triggers a repaint if the page is visible.
 */
void app_status_screen_scroll_messages(int delta);

/** Backwards-compat shortcut: show the status page for hold_ms. */
esp_err_t app_status_screen_request_status(uint32_t hold_ms);

#ifdef __cplusplus
}
#endif
