/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Coarse-grained mutex used to coordinate access to a SPI bus that is
 *        shared between drivers with incompatible transaction models — most
 *        notably the polling-based `sdspi_host` (SD card) and the queued/DMA
 *        `esp_lcd_panel_io_spi` (LCD).
 *
 * Even though `spi_master` serializes per-device transactions internally, the
 * combination of a polling driver and a queued/DMA driver on the same host
 * has a known race: when the queued/DMA transaction's interrupt releases the
 * bus lock, the polling driver can enter `spi_hal_setup_trans` before the
 * hardware `usr` bit has fully cleared, triggering the assert
 *   `assert failed: spi_hal_setup_trans (spi_ll_get_running_cmd(hw) == 0)`
 *
 * Wrapping critical sections (the entire SD I/O burst on one side, the entire
 * LCD draw_bitmap call on the other) in this mutex prevents any chance of
 * interleaving and avoids the race.
 *
 * The mutex is recursive (the same task may take it any number of times) and
 * lazily created on first use.
 */
esp_err_t spi_bus_arbiter_lock(uint32_t timeout_ms);

/**
 * @brief Release a lock previously taken by `spi_bus_arbiter_lock()`.
 */
void spi_bus_arbiter_unlock(void);

#ifdef __cplusplus
}
#endif
