/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Calibration values for an XPT2046 resistive touch panel.
 *
 * Convention (matching the TFT_eSPI library's `setTouch()` calData layout):
 *   raw_x_min/raw_x_max : raw 12-bit ADC range for the panel X axis.
 *   raw_y_min/raw_y_max : raw 12-bit ADC range for the panel Y axis.
 *   swap_xy : if true, swap raw X and Y before mapping (typical in landscape).
 *   invert_x / invert_y : flip the corresponding screen axis after mapping.
 */
typedef struct {
    uint16_t raw_x_min;
    uint16_t raw_x_max;
    uint16_t raw_y_min;
    uint16_t raw_y_max;
    bool     swap_xy;
    bool     invert_x;
    bool     invert_y;
} app_touch_xpt2046_cal_t;

typedef struct {
    spi_host_device_t host;          /* SPI bus already initialized for the LCD. */
    int               cs_gpio;       /* GPIO connected to T_CS.                  */
    int               irq_gpio;      /* -1 if PENIRQ is not wired (poll only).   */
    int               screen_width;  /* Screen pixels in the current orientation.*/
    int               screen_height;
    app_touch_xpt2046_cal_t calibration;
} app_touch_xpt2046_config_t;

typedef void (*app_touch_xpt2046_cb_t)(int x, int y, int pressure, void *user_ctx);

/**
 * Bring up an XPT2046 polling driver. The chip shares the LCD's SPI bus; the
 * driver attaches a new device at 1 MHz with mode 0. A low-priority polling
 * task samples the panel every ~30 ms, debounces, and fires `cb` on each
 * detected tap (press -> release sequence) with mapped screen coordinates.
 */
esp_err_t app_touch_xpt2046_init(const app_touch_xpt2046_config_t *config,
                                 app_touch_xpt2046_cb_t cb,
                                 void *user_ctx);

#ifdef __cplusplus
}
#endif
