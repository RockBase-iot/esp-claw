/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_message_inbox.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "msg_inbox";

typedef struct {
    SemaphoreHandle_t lock;
    app_message_inbox_entry_t ring[APP_MESSAGE_INBOX_CAPACITY];
    size_t head;          // index of next write slot
    size_t count;         // 0..APP_MESSAGE_INBOX_CAPACITY
    size_t unread;
    app_message_inbox_change_cb_t cb;
    void *cb_ctx;
} app_message_inbox_state_t;

static app_message_inbox_state_t s_inbox;

static esp_err_t inbox_lock_take(void)
{
    if (!s_inbox.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_inbox.lock, pdMS_TO_TICKS(200)) == pdTRUE
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void inbox_lock_give(void)
{
    if (s_inbox.lock) {
        xSemaphoreGive(s_inbox.lock);
    }
}

esp_err_t app_message_inbox_init(void)
{
    if (s_inbox.lock) {
        return ESP_OK;
    }
    s_inbox.lock = xSemaphoreCreateMutex();
    if (!s_inbox.lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_inbox.ring, 0, sizeof(s_inbox.ring));
    s_inbox.head = 0;
    s_inbox.count = 0;
    s_inbox.unread = 0;
    return ESP_OK;
}

esp_err_t app_message_inbox_record(const char *channel,
                                   const char *sender,
                                   const char *text,
                                   int64_t timestamp_ms)
{
    if (!s_inbox.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!text) {
        text = "";
    }
    if (timestamp_ms <= 0) {
        timestamp_ms = esp_timer_get_time() / 1000LL;
    }

    if (inbox_lock_take() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    app_message_inbox_entry_t *slot = &s_inbox.ring[s_inbox.head];
    memset(slot, 0, sizeof(*slot));
    slot->timestamp_ms = timestamp_ms;
    strlcpy(slot->channel, channel ? channel : "?", sizeof(slot->channel));
    strlcpy(slot->sender,  sender  ? sender  : "",  sizeof(slot->sender));
    strlcpy(slot->text,    text,                   sizeof(slot->text));

    s_inbox.head = (s_inbox.head + 1) % APP_MESSAGE_INBOX_CAPACITY;
    if (s_inbox.count < APP_MESSAGE_INBOX_CAPACITY) {
        s_inbox.count++;
    }
    if (s_inbox.unread < APP_MESSAGE_INBOX_CAPACITY) {
        s_inbox.unread++;
    }

    app_message_inbox_change_cb_t cb = s_inbox.cb;
    void *cb_ctx = s_inbox.cb_ctx;
    inbox_lock_give();

    ESP_LOGI(TAG, "[%s] %s: %.40s%s",
             channel ? channel : "?",
             sender  ? sender  : "",
             text,
             strlen(text) > 40 ? "..." : "");

    if (cb) {
        cb(cb_ctx);
    }
    return ESP_OK;
}

size_t app_message_inbox_count(void)
{
    if (inbox_lock_take() != ESP_OK) {
        return 0;
    }
    size_t n = s_inbox.count;
    inbox_lock_give();
    return n;
}

size_t app_message_inbox_unread_count(void)
{
    if (inbox_lock_take() != ESP_OK) {
        return 0;
    }
    size_t n = s_inbox.unread;
    inbox_lock_give();
    return n;
}

void app_message_inbox_mark_all_read(void)
{
    if (inbox_lock_take() != ESP_OK) {
        return;
    }
    bool changed = (s_inbox.unread != 0);
    s_inbox.unread = 0;
    app_message_inbox_change_cb_t cb = s_inbox.cb;
    void *cb_ctx = s_inbox.cb_ctx;
    inbox_lock_give();
    if (changed && cb) {
        cb(cb_ctx);
    }
}

void app_message_inbox_clear(void)
{
    if (inbox_lock_take() != ESP_OK) {
        return;
    }
    memset(s_inbox.ring, 0, sizeof(s_inbox.ring));
    s_inbox.head = 0;
    s_inbox.count = 0;
    s_inbox.unread = 0;
    app_message_inbox_change_cb_t cb = s_inbox.cb;
    void *cb_ctx = s_inbox.cb_ctx;
    inbox_lock_give();
    if (cb) {
        cb(cb_ctx);
    }
}

bool app_message_inbox_get(size_t index, app_message_inbox_entry_t *out_entry)
{
    if (!out_entry) {
        return false;
    }
    if (inbox_lock_take() != ESP_OK) {
        return false;
    }
    bool ok = false;
    if (index < s_inbox.count) {
        // index 0 = newest = ring[head-1]; index k = ring[head-1-k]
        size_t pos = (s_inbox.head + APP_MESSAGE_INBOX_CAPACITY - 1 - index)
                     % APP_MESSAGE_INBOX_CAPACITY;
        *out_entry = s_inbox.ring[pos];
        ok = true;
    }
    inbox_lock_give();
    return ok;
}

void app_message_inbox_set_change_callback(app_message_inbox_change_cb_t cb, void *user_ctx)
{
    if (inbox_lock_take() != ESP_OK) {
        s_inbox.cb = cb;
        s_inbox.cb_ctx = user_ctx;
        return;
    }
    s_inbox.cb = cb;
    s_inbox.cb_ctx = user_ctx;
    inbox_lock_give();
}
