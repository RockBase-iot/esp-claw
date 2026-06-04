/*
 * SPDX-FileCopyrightText:2026 RockBase-iot Technology (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialise the custom logo subsystem.
 *
 * If a custom SVG logo exists at LOGO_SVG_PATH, it is rasterised and
 * displayed on the LCD, replacing the emote idle animation.
 *
 * Call once after emote_start().
 */
esp_err_t logo_start(void);

/**
 * @brief  Re-read the SVG from storage and update the LCD.
 *
 * Called by the HTTP API after a new logo is uploaded.
 * Acquires the display arbiter (LOGO owner) and pushes the bitmap.
 */
esp_err_t logo_refresh(void);

/**
 * @brief  Stop displaying the custom logo and restore emote idle animation.
 *
 * Releases the display arbiter so the emote regains ownership.
 */
esp_err_t logo_stop(void);

/**
 * @brief  Returns true if a custom SVG logo file exists on storage.
 */
bool logo_file_exists(void);

/**
 * @brief  Returns true if the logo is currently displayed on the LCD.
 */
bool logo_is_active(void);

/**
 * @brief  Get the filesystem path where the custom logo SVG is stored.
 */
const char *logo_get_path(void);

#ifdef __cplusplus
}
#endif
