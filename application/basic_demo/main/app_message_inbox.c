/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_message_inbox.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "msg_inbox";

#define INBOX_PERSIST_FILE_NAME   "messages.jsonl"
#define INBOX_PERSIST_MAX_BYTES   (256 * 1024)
#define INBOX_PERSIST_PATH_LEN    160

typedef struct {
    SemaphoreHandle_t lock;
    app_message_inbox_entry_t ring[APP_MESSAGE_INBOX_CAPACITY];
    size_t head;          // index of next write slot
    size_t count;         // 0..APP_MESSAGE_INBOX_CAPACITY
    size_t unread;
    app_message_inbox_change_cb_t cb;
    void *cb_ctx;
    char persist_dir[INBOX_PERSIST_PATH_LEN];
    char persist_path[INBOX_PERSIST_PATH_LEN];
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

/* ----- Persistence helpers ------------------------------------------- */

static void inbox_json_escape(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    size_t o = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] != '\0' && o + 2 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        const char *esc = NULL;
        char esc_buf[8];
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b";  break;
        case '\f': esc = "\\f";  break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c < 0x20) {
                snprintf(esc_buf, sizeof(esc_buf), "\\u%04x", c);
                esc = esc_buf;
            }
            break;
        }
        if (esc) {
            size_t l = strlen(esc);
            if (o + l + 1 >= dst_size) {
                break;
            }
            memcpy(dst + o, esc, l);
            o += l;
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static void inbox_persist_rotate_if_needed(void)
{
    if (s_inbox.persist_path[0] == '\0') {
        return;
    }
    struct stat st;
    if (stat(s_inbox.persist_path, &st) != 0) {
        return;
    }
    if ((size_t)st.st_size < INBOX_PERSIST_MAX_BYTES) {
        return;
    }
    char rotated[INBOX_PERSIST_PATH_LEN + 4];
    snprintf(rotated, sizeof(rotated), "%s.1", s_inbox.persist_path);
    /* Best effort: remove old backup, then rename. Failures are non-fatal. */
    remove(rotated);
    if (rename(s_inbox.persist_path, rotated) != 0) {
        ESP_LOGW(TAG, "persist rotate failed: %s", strerror(errno));
    }
}

static void inbox_persist_write(const app_message_inbox_entry_t *e)
{
    if (!e || s_inbox.persist_path[0] == '\0') {
        return;
    }
    inbox_persist_rotate_if_needed();
    FILE *fp = fopen(s_inbox.persist_path, "ab");
    if (!fp) {
        ESP_LOGW(TAG, "persist open failed: %s", strerror(errno));
        return;
    }
    char ch_buf[APP_MESSAGE_INBOX_CHANNEL_LEN * 2];
    char sd_buf[APP_MESSAGE_INBOX_SENDER_LEN * 2];
    char tx_buf[APP_MESSAGE_INBOX_TEXT_LEN * 2];
    inbox_json_escape(e->channel, ch_buf, sizeof(ch_buf));
    inbox_json_escape(e->sender,  sd_buf, sizeof(sd_buf));
    inbox_json_escape(e->text,    tx_buf, sizeof(tx_buf));
    fprintf(fp,
            "{\"ts\":%lld,\"channel\":\"%s\",\"sender\":\"%s\",\"text\":\"%s\"}\n",
            (long long)e->timestamp_ms, ch_buf, sd_buf, tx_buf);
    fclose(fp);
}

esp_err_t app_message_inbox_set_persist_dir(const char *dir)
{
    if (!s_inbox.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (inbox_lock_take() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (!dir || dir[0] == '\0') {
        s_inbox.persist_dir[0] = '\0';
        s_inbox.persist_path[0] = '\0';
        inbox_lock_give();
        return ESP_OK;
    }
    /* Best-effort mkdir(parent); ignore EEXIST. */
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "persist mkdir(%s) failed: %s", dir, strerror(errno));
    }
    strlcpy(s_inbox.persist_dir, dir, sizeof(s_inbox.persist_dir));
    int n = snprintf(s_inbox.persist_path, sizeof(s_inbox.persist_path),
                     "%s/%s", dir, INBOX_PERSIST_FILE_NAME);
    if (n <= 0 || (size_t)n >= sizeof(s_inbox.persist_path)) {
        s_inbox.persist_path[0] = '\0';
        inbox_lock_give();
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "persisting messages to %s", s_inbox.persist_path);
    inbox_lock_give();
    return ESP_OK;
}

/* ----- Persistence loader -------------------------------------------- */

static bool inbox_json_unescape(const char *in, size_t in_len,
                                char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return false;
    }
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_size; i++) {
        char c = in[i];
        if (c != '\\') {
            out[o++] = c;
            continue;
        }
        if (i + 1 >= in_len) {
            break;
        }
        char n = in[++i];
        switch (n) {
        case '"':  out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; break;
        case '/':  out[o++] = '/';  break;
        case 'b':  out[o++] = '\b'; break;
        case 'f':  out[o++] = '\f'; break;
        case 'n':  out[o++] = '\n'; break;
        case 'r':  out[o++] = '\r'; break;
        case 't':  out[o++] = '\t'; break;
        case 'u': {
            if (i + 4 >= in_len) {
                break;
            }
            unsigned code = 0;
            for (int k = 1; k <= 4; k++) {
                char hc = in[i + k];
                unsigned d;
                if (hc >= '0' && hc <= '9') {
                    d = hc - '0';
                } else if (hc >= 'a' && hc <= 'f') {
                    d = 10 + hc - 'a';
                } else if (hc >= 'A' && hc <= 'F') {
                    d = 10 + hc - 'A';
                } else {
                    code = 0xFFFF;
                    break;
                }
                code = (code << 4) | d;
            }
            i += 4;
            if (code < 0x80) {
                out[o++] = (char)code;
            } else {
                /* Non-ASCII unicode escapes are rare in our writes; emit '?' */
                out[o++] = '?';
            }
            break;
        }
        default: out[o++] = n; break;
        }
    }
    out[o] = '\0';
    return true;
}

/* Find `"key":"...value..."` in a single line (no nested objects).
 * Returns true and fills out_value (NUL terminated). */
static bool inbox_parse_string_field(const char *line, const char *key,
                                     char *out, size_t out_size)
{
    char pat[24];
    int n = snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pat)) {
        return false;
    }
    const char *p = strstr(line, pat);
    if (!p) {
        return false;
    }
    p += n;
    /* find unescaped closing quote */
    const char *end = p;
    while (*end) {
        if (*end == '\\' && end[1] != '\0') {
            end += 2;
            continue;
        }
        if (*end == '"') {
            break;
        }
        end++;
    }
    if (*end != '"') {
        return false;
    }
    return inbox_json_unescape(p, (size_t)(end - p), out, out_size);
}

static bool inbox_parse_int_field(const char *line, const char *key, int64_t *out)
{
    char pat[24];
    int n = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(pat)) {
        return false;
    }
    const char *p = strstr(line, pat);
    if (!p) {
        return false;
    }
    p += n;
    while (*p == ' ') {
        p++;
    }
    char *endp = NULL;
    long long v = strtoll(p, &endp, 10);
    if (endp == p) {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

/* Insert a parsed entry directly into the ring without persisting or
 * touching the unread counter. Caller holds the inbox lock. */
static void inbox_push_loaded_locked(const app_message_inbox_entry_t *e)
{
    app_message_inbox_entry_t *slot = &s_inbox.ring[s_inbox.head];
    *slot = *e;
    s_inbox.head = (s_inbox.head + 1) % APP_MESSAGE_INBOX_CAPACITY;
    if (s_inbox.count < APP_MESSAGE_INBOX_CAPACITY) {
        s_inbox.count++;
    }
}

esp_err_t app_message_inbox_load_persisted(size_t max_load)
{
    if (!s_inbox.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[INBOX_PERSIST_PATH_LEN];
    if (inbox_lock_take() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_inbox.persist_path[0] == '\0') {
        inbox_lock_give();
        ESP_LOGW(TAG, "load skipped: persist dir not set");
        return ESP_OK;
    }
    strlcpy(path, s_inbox.persist_path, sizeof(path));
    inbox_lock_give();

    if (max_load == 0 || max_load > APP_MESSAGE_INBOX_CAPACITY) {
        max_load = APP_MESSAGE_INBOX_CAPACITY;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (errno == ENOENT) {
            ESP_LOGI(TAG, "load: %s does not exist yet", path);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "load: fopen(%s) failed: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    /* Tail-read: only the last ~64 KiB matter even for a huge file. */
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    long fsize = ftell(fp);
    if (fsize <= 0) {
        fclose(fp);
        return ESP_OK;
    }
    const long tail_window = 64 * 1024;
    long offset = (fsize > tail_window) ? (fsize - tail_window) : 0;
    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    size_t to_read = (size_t)(fsize - offset);
    char *buf = malloc(to_read + 1);
    if (!buf) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, to_read, fp);
    fclose(fp);
    buf[got] = '\0';

    /* If we started mid-file, drop the first partial line. */
    char *cursor = buf;
    if (offset > 0) {
        char *nl = strchr(cursor, '\n');
        if (nl) {
            cursor = nl + 1;
        } else {
            free(buf);
            return ESP_OK;
        }
    }

    /* Collect line start pointers (in place; convert '\n' to '\0'). */
    char *lines[APP_MESSAGE_INBOX_CAPACITY * 4];
    size_t nlines = 0;
    char *p = cursor;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (*p == '{') {
            lines[nlines % (sizeof(lines) / sizeof(lines[0]))] = p;
            nlines++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    size_t ring_cap = sizeof(lines) / sizeof(lines[0]);
    size_t avail = (nlines < ring_cap) ? nlines : ring_cap;
    size_t take  = (avail < max_load)  ? avail  : max_load;
    if (take == 0) {
        free(buf);
        ESP_LOGI(TAG, "load: no parsable lines in %s", path);
        return ESP_OK;
    }

    /* Iterate the most recent `take` lines in chronological order. */
    size_t loaded = 0;
    if (inbox_lock_take() != ESP_OK) {
        free(buf);
        return ESP_ERR_TIMEOUT;
    }
    size_t base = (nlines >= ring_cap) ? (nlines % ring_cap) : 0;
    /* The newest `take` lines correspond to indices [avail-take, avail). */
    for (size_t k = avail - take; k < avail; k++) {
        size_t li = (nlines >= ring_cap) ? ((base + k) % ring_cap) : k;
        const char *line = lines[li];
        app_message_inbox_entry_t entry = {0};
        int64_t ts = 0;
        (void)inbox_parse_int_field(line, "ts", &ts);
        entry.timestamp_ms = ts;
        if (!inbox_parse_string_field(line, "channel", entry.channel, sizeof(entry.channel))) {
            strlcpy(entry.channel, "?", sizeof(entry.channel));
        }
        inbox_parse_string_field(line, "sender", entry.sender, sizeof(entry.sender));
        inbox_parse_string_field(line, "text",   entry.text,   sizeof(entry.text));
        inbox_push_loaded_locked(&entry);
        loaded++;
    }
    inbox_lock_give();
    free(buf);
    ESP_LOGI(TAG, "load: restored %u messages from %s (file=%ld bytes)",
             (unsigned)loaded, path, fsize);
    return ESP_OK;
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
    app_message_inbox_entry_t snapshot = *slot;
    inbox_lock_give();

    inbox_persist_write(&snapshot);

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
