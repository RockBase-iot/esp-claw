/*
 * SPDX-FileCopyrightText: 2026 RockBase IoT (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

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

typedef struct {
    const char *sd_root;                    /* SD mount point, typically "/sdcard" */
    const app_sd_mirror_entry_t *entries;   /* array of mirror entries             */
    size_t entry_count;
} app_sd_mirror_config_t;

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
 * @brief Bidirectionally mirror an entire directory (non-recursive, regular
 *        files only) between FATFS and SD using the same per-file newer-mtime
 *        rule used by app_sd_mirror_sync().
 *
 * Files present on either side are unioned: each file is independently
 * compared/copied. Files deleted on one side are NOT removed from the other
 * (this avoids losing user data after a re-flash that ships an older
 * baseline). Subdirectories are skipped.
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
