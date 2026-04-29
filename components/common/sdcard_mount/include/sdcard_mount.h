/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
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
 * @brief  Attempt to mount the on-board SD card via the board manager
 *         `fs_sdcard` device entry. Safe to call when no card is inserted —
 *         in that case it logs a warning and returns ESP_FAIL without
 *         leaving any partial state behind.
 *
 * @return
 *  - ESP_OK if the SD card is mounted (mount point reachable on VFS).
 *  - ESP_ERR_NOT_FOUND if the board has no `fs_sdcard` device declared.
 *  - ESP_FAIL if the device exists but mount failed (no card, bad card, ...).
 */
esp_err_t sdcard_mount_init(void);

/**
 * @brief  Returns true if the SD card has been successfully mounted.
 */
bool sdcard_mount_is_mounted(void);

/**
 * @brief  Returns the VFS mount point (e.g. "/sdcard"), or NULL if not
 *         mounted.
 */
const char *sdcard_mount_get_mount_point(void);

/**
 * @brief  Query free / total bytes on the SD card filesystem.
 *         Returns ESP_ERR_INVALID_STATE if the SD card is not mounted.
 */
esp_err_t sdcard_mount_get_usage(uint64_t *out_total_bytes,
                                 uint64_t *out_free_bytes);

/**
 * @brief  Probe the card with a tiny VFS call. If the probe fails (typical
 *         after the user yanks the card) the FAT VFS is unmounted, the
 *         underlying SDSPI device is released, and the internal `mounted`
 *         flag is cleared so subsequent `sdcard_mount_is_mounted()` calls
 *         return false.
 *
 * @return true if the card responded successfully, false if it has been
 *         removed / become unresponsive (or was never mounted).
 */
bool sdcard_mount_check_alive(void);

/**
 * @brief  Force unmount the SD card, releasing the SDSPI device. Safe to
 *         call when not mounted (no-op).
 */
void sdcard_mount_force_unmount(void);

/**
 * @brief  "Probe-or-mount" callback intended for use as the http_server
 *         extra-mount alive_cb. Behavior:
 *         - If the card is currently mounted, runs the same probe as
 *           `sdcard_mount_check_alive()` and returns its result (so a
 *           hot-removed card is detected and unmounted automatically).
 *         - If the card is not mounted, attempts a fresh mount once,
 *           rate-limited by an internal cooldown so repeated web refreshes
 *           do not hammer the SDSPI bus. Returns true if the new mount
 *           succeeded.
 */
bool sdcard_mount_alive_or_remount(void);

#ifdef __cplusplus
}
#endif
