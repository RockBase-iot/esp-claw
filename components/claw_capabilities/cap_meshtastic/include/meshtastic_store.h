/*
 * SPDX-FileCopyrightText: 2026 Chengdu RockBase Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configuration for the persistent Meshtastic message store.
 */
typedef struct {
    const char *base_path;    /**< VFS mount point, e.g. "/fatfs" or "/sdcard" */
    size_t max_file_bytes;    /**< Max file size; 0 = unlimited (SD card) */
} meshtastic_store_config_t;

/**
 * Optional callbacks to take/release a coarse SPI bus lock around every block
 * of file I/O. Required when the backing filesystem (e.g. an SD card) shares a
 * SPI host with another driver such as an LCD, so the polling SDSPI driver
 * cannot interleave with queued/DMA LCD transactions and trigger
 *   assert failed: spi_hal_setup_trans (spi_ll_get_running_cmd(hw) == 0)
 */
typedef bool (*meshtastic_store_bus_lock_fn)(void);
typedef void (*meshtastic_store_bus_unlock_fn)(bool was_locked);

/** Install (or clear, with NULL) the SPI bus lock callbacks. */
void meshtastic_store_set_bus_lock(meshtastic_store_bus_lock_fn lock,
                                   meshtastic_store_bus_unlock_fn unlock);

/**
 * Initialise the persistent store.  Creates the subdirectory and file if
 * they do not exist.  Safe to call again to reconfigure the path. Calling it
 * again with the same resolved path and limit is a cheap no-op (no I/O).
 */
esp_err_t meshtastic_store_init(const meshtastic_store_config_t *config);

/**
 * Append a received text message as one JSONL line.
 */
esp_err_t meshtastic_store_append(uint32_t from, const char *from_id,
                                  uint32_t channel, uint32_t packet_id,
                                  const char *text, int64_t ts_ms);

/**
 * Read stored messages into a cJSON array (newest first).
 * @param array     Pre-created cJSON array; objects are appended.
 * @param max_count Maximum number of messages to return (0 = all).
 */
esp_err_t meshtastic_store_read(cJSON *array, size_t max_count);

/** Delete the store file and reset counters. */
esp_err_t meshtastic_store_clear(void);

/** Number of messages currently stored. */
size_t meshtastic_store_count(void);

/** Current store file size in bytes. */
size_t meshtastic_store_file_size(void);

/** Absolute path to the store file (valid after init). */
const char *meshtastic_store_path(void);

/** True if the store has been initialised. */
bool meshtastic_store_is_ready(void);

#ifdef __cplusplus
}
#endif
