/*
 * SPDX-FileCopyrightText: 2026 RockBase-iot Technology (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Rasterize an SVG file to an RGB565 pixel buffer.
 *
 * The SVG is scaled to fit within (w x h) while preserving aspect ratio.
 * The returned buffer is heap-allocated and must be freed by the caller.
 *
 * @param[in]  path        Filesystem path to the SVG file.
 * @param[in]  w           Target width in pixels.
 * @param[in]  h           Target height in pixels.
 * @param[out] out_buf     Receives a pointer to the RGB565 buffer (w*h elements).
 * @param[in]  swap_bytes  If true, each 16-bit pixel is byte-swapped (big-endian).
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
esp_err_t nanosvg_rasterize_file_rgb565(const char *path,
                                         int w,
                                         int h,
                                         uint16_t **out_buf,
                                         bool swap_bytes);

/**
 * @brief  Rasterize SVG from a memory buffer to an RGB565 pixel buffer.
 *
 * @param[in]  svg_data    Null-terminated SVG source string.
 * @param[in]  w           Target width in pixels.
 * @param[in]  h           Target height in pixels.
 * @param[out] out_buf     Receives a pointer to the RGB565 buffer (w*h elements).
 * @param[in]  swap_bytes  If true, each 16-bit pixel is byte-swapped (big-endian).
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
esp_err_t nanosvg_rasterize_buf_rgb565(const char *svg_data,
                                        int w,
                                        int h,
                                        uint16_t **out_buf,
                                        bool swap_bytes);

#ifdef __cplusplus
}
#endif
