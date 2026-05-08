/*
 * SPDX-FileCopyrightText: 2026 RockBase IoT (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One bidirectional mirror entry between FATFS and SD.
 *
 * For each entry, app_sd_mirror_sync() compares the file on FATFS against the
 * file on SD by modification time and copies the newer side onto the older
 * one. If the file only exists on one side it is copied to the other side.
 * Missing parent directories are created automatically. Files that do not
 * exist on either side are silently skipped (treated as "no user data yet").
 */
typedef struct {
    const char *fatfs_path;   /* absolute path on the FATFS partition,  e.g. "/fatfs/memory/soul.md" */
    const char *sd_relpath;   /* path under the SD root (no leading '/'), e.g. "memory/soul.md"     */
} app_sd_mirror_entry_t;

/**
 * @brief Optional callbacks used to take/release a coarse SPI bus lock around
 *        every block of SD-side I/O. Required when the SD card shares a SPI
 *        host with another driver (e.g. an LCD via esp_lcd_panel_io_spi) so
 *        the polling SDSPI driver cannot interleave with queued/DMA LCD
 *        transactions and trigger
 *        `assert failed: spi_hal_setup_trans (spi_ll_get_running_cmd(hw) == 0)`.
 *
 * The lock is held only during a few read/write/stat calls at a time (file
 * granularity), not for the entire mirror, so display refresh can interleave
 * between files.
 */
typedef bool (*app_sd_mirror_bus_lock_fn)(void);
typedef void (*app_sd_mirror_bus_unlock_fn)(bool was_locked);

typedef struct {
    const char *sd_root;                    /* SD mount point, typically "/sdcard" */
    const app_sd_mirror_entry_t *entries;   /* array of mirror entries             */
    size_t entry_count;
    app_sd_mirror_bus_lock_fn   bus_lock;   /* may be NULL */
    app_sd_mirror_bus_unlock_fn bus_unlock; /* may be NULL */
} app_sd_mirror_config_t;

/**
 * @brief Install global SPI bus lock callbacks used by app_sd_mirror_sync_dir()
 *        (which has no per-call config struct). Pass NULL to disable.
 *        Same semantics as the bus_lock/bus_unlock fields of
 *        app_sd_mirror_config_t.
 */
void app_sd_mirror_set_bus_lock(app_sd_mirror_bus_lock_fn lock,
                                app_sd_mirror_bus_unlock_fn unlock);

/**
 * @brief One-shot bidirectional sync. Newer-mtime side wins.
 *
 * Safe to call when the SD card is missing — returns ESP_ERR_INVALID_STATE
 * after logging. Per-entry failures are logged but do not stop the sweep.
 *
 * @return ESP_OK if the sweep ran (even if some entries failed individually).
 *         ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE otherwise.
 */
esp_err_t app_sd_mirror_sync(const app_sd_mirror_config_t *config);

/**
 * @brief Bidirectionally mirror an entire directory tree (recursive, regular
 *        files only) between FATFS and SD using the same per-file newer-mtime
 *        rule used by app_sd_mirror_sync().
 *
 * Files present on either side are unioned: each file is independently
 * compared/copied. Files deleted on one side are NOT removed from the other
 * (this avoids losing user data after a re-flash that ships an older
 * baseline). Subdirectories are walked recursively.
 *
 * @param sd_root      SD mount point (e.g. "/sdcard"). Must exist as a directory.
 * @param fatfs_dir    Absolute FATFS directory (e.g. "/fatfs/skills").
 * @param sd_subdir    Path under @p sd_root (e.g. "skills"). May be "" to use the root.
 * @return ESP_OK if the sweep ran (per-file failures are logged).
 */
esp_err_t app_sd_mirror_sync_dir(const char *sd_root,
                                 const char *fatfs_dir,
                                 const char *sd_subdir);

#ifdef __cplusplus
}
#endif
