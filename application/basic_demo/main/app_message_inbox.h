/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MESSAGE_INBOX_CAPACITY      32
#define APP_MESSAGE_INBOX_CHANNEL_LEN   16
#define APP_MESSAGE_INBOX_SENDER_LEN    48
#define APP_MESSAGE_INBOX_TEXT_LEN      192

typedef struct {
    int64_t timestamp_ms;
    char    channel[APP_MESSAGE_INBOX_CHANNEL_LEN];   // "feishu" / "telegram" / "qq" / "wechat"
    char    sender[APP_MESSAGE_INBOX_SENDER_LEN];
    char    text[APP_MESSAGE_INBOX_TEXT_LEN];
} app_message_inbox_entry_t;

typedef void (*app_message_inbox_change_cb_t)(void *user_ctx);

esp_err_t app_message_inbox_init(void);

/** Append a message; oldest entries are dropped when full. */
esp_err_t app_message_inbox_record(const char *channel,
                                   const char *sender,
                                   const char *text,
                                   int64_t timestamp_ms);

size_t app_message_inbox_count(void);
size_t app_message_inbox_unread_count(void);
void   app_message_inbox_mark_all_read(void);
void   app_message_inbox_clear(void);

/** Get entry by index, 0 = newest. Returns false if out of range. */
bool app_message_inbox_get(size_t index, app_message_inbox_entry_t *out_entry);

/** Single-slot change callback (UI redraw). */
void app_message_inbox_set_change_callback(app_message_inbox_change_cb_t cb, void *user_ctx);

/**
 * Optional: persist every recorded message as a JSONL line at
 * `<dir>/messages.jsonl`. Pass NULL to disable. The directory is
 * created on demand. When the file exceeds ~256 KiB it is rotated
 * to `messages.jsonl.1` (one backup). Writes happen synchronously
 * from the calling task; keep IM bursts modest or push to SD.
 */
esp_err_t app_message_inbox_set_persist_dir(const char *dir);

/**
 * Load up to `max_load` (0 means "use ring capacity") most-recent persisted
 * messages from the file configured via `app_message_inbox_set_persist_dir()`.
 * Entries are inserted into the in-memory ring in chronological order so the
 * newest message ends up at index 0 of `app_message_inbox_get()`.
 *
 * Loaded entries are not re-persisted, do not increment the unread counter,
 * and do not trigger the change callback. The caller may issue a UI refresh.
 * Returns ESP_OK on success or when no persist file exists yet.
 */
esp_err_t app_message_inbox_load_persisted(size_t max_load);

#ifdef __cplusplus
}
#endif
