/*
 * SPDX-FileCopyrightText: 2026 Chengdu RockBase Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "meshtastic_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mesh_store";

#define STORE_SUBDIR   "meshtastic"
#define STORE_FILENAME "messages.jsonl"
#define STORE_LINE_MAX 4096

static struct {
    bool ready;
    char file_path[192];
    size_t max_bytes;
    size_t msg_count;
    size_t file_size;
    SemaphoreHandle_t lock;
    meshtastic_store_bus_lock_fn bus_lock;
    meshtastic_store_bus_unlock_fn bus_unlock;
} s_store = {0};

/* ---- internal helpers ---- */

void meshtastic_store_set_bus_lock(meshtastic_store_bus_lock_fn lock,
                                   meshtastic_store_bus_unlock_fn unlock)
{
    s_store.bus_lock = lock;
    s_store.bus_unlock = unlock;
}

static bool store_bus_lock(void)
{
    return s_store.bus_lock ? s_store.bus_lock() : false;
}

static void store_bus_unlock(bool was_locked)
{
    if (s_store.bus_unlock) {
        s_store.bus_unlock(was_locked);
    }
}

static esp_err_t ensure_directory(const char *dir_path)
{
    struct stat st;
    if (stat(dir_path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "%s exists but is not a directory", dir_path);
        return ESP_FAIL;
    }
    if (mkdir(dir_path, 0755) != 0) {
        if (errno == EEXIST) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "mkdir(%s) failed", dir_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static char *read_line_alloc(FILE *fp, size_t *out_len)
{
    if (!fp) {
        return NULL;
    }

    size_t cap = 256;
    size_t len = 0;
    bool got_any = false;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }

    int ch = EOF;
    while ((ch = fgetc(fp)) != EOF) {
        got_any = true;
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }

        if (len + 1 >= cap) {
            size_t new_cap = cap * 2;
            if (new_cap > STORE_LINE_MAX) {
                while (ch != '\n' && ch != EOF) {
                    ch = fgetc(fp);
                }
                break;
            }
            char *tmp = realloc(buf, new_cap);
            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
            cap = new_cap;
        }
        buf[len++] = (char)ch;
    }

    if (!got_any && ch == EOF) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (out_len) {
        *out_len = len;
    }
    return buf;
}

static size_t get_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (size_t)st.st_size;
}

static size_t count_lines(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return 0;
    }
    size_t count = 0;
    char *line = NULL;
    while ((line = read_line_alloc(fp, NULL)) != NULL) {
        if (line[0] == '{') {
            count++;
        }
        free(line);
    }
    fclose(fp);
    return count;
}

static esp_err_t rotate_store_file(const char *tmp_path, const char *bak_path)
{
    if (rename(tmp_path, s_store.file_path) == 0) {
        return ESP_OK;
    }

    if (rename(s_store.file_path, bak_path) != 0) {
        ESP_LOGW(TAG, "rotation: backup rename failed");
        remove(tmp_path);
        return ESP_FAIL;
    }

    if (rename(tmp_path, s_store.file_path) != 0) {
        ESP_LOGW(TAG, "rotation: swap-in failed, restoring backup");
        (void)rename(bak_path, s_store.file_path);
        remove(tmp_path);
        return ESP_FAIL;
    }

    remove(bak_path);
    return ESP_OK;
}

static void update_cached_stats(void)
{
    s_store.file_size = get_file_size(s_store.file_path);
    s_store.msg_count = count_lines(s_store.file_path);
}

static void rotate_if_needed(void)
{
    if (s_store.max_bytes == 0 || s_store.file_size <= s_store.max_bytes) {
        return;
    }

    ESP_LOGI(TAG, "file size %u exceeds limit %u, rotating",
             (unsigned)s_store.file_size, (unsigned)s_store.max_bytes);

    /*
     * Simple rotation: keep the last ~50% of the file.
     * Read from the midpoint, write to a temp file, then replace.
     */
    char tmp_path[sizeof(s_store.file_path) + 8];
    char bak_path[sizeof(s_store.file_path) + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", s_store.file_path);
    snprintf(bak_path, sizeof(bak_path), "%s.bak", s_store.file_path);
    (void)remove(tmp_path);

    FILE *src = fopen(s_store.file_path, "r");
    FILE *dst = fopen(tmp_path, "w");
    if (!src || !dst) {
        if (src) {
            fclose(src);
        }
        if (dst) {
            fclose(dst);
        }
        ESP_LOGW(TAG, "rotation: open failed, keeping old file");
        (void)remove(tmp_path);
        return;
    }

    /* Skip to the midpoint of the file. */
    long half = (long)(s_store.file_size / 2);
    fseek(src, half, SEEK_SET);
    /* Discard partial line. */
    char *line = read_line_alloc(src, NULL);
    if (!line) {
        fclose(src);
        fclose(dst);
        (void)remove(tmp_path);
        return;
    }
    free(line);

    /* Copy remaining complete lines. */
    while ((line = read_line_alloc(src, NULL)) != NULL) {
        if (line[0] == '{') {
            fputs(line, dst);
            fputc('\n', dst);
        }
        free(line);
    }
    fclose(src);
    fclose(dst);

    if (rotate_store_file(tmp_path, bak_path) != ESP_OK) {
        update_cached_stats();
        return;
    }

    update_cached_stats();
    ESP_LOGI(TAG, "rotation done: %u messages, %u bytes",
             (unsigned)s_store.msg_count, (unsigned)s_store.file_size);
}

/* ---- public API ---- */

esp_err_t meshtastic_store_init(const meshtastic_store_config_t *config)
{
    if (!config || !config->base_path || !config->base_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_store.lock) {
        s_store.lock = xSemaphoreCreateMutex();
        if (!s_store.lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_store.lock, portMAX_DELAY);

    char dir_path[160];
    char new_file_path[sizeof(s_store.file_path)];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", config->base_path, STORE_SUBDIR);
    snprintf(new_file_path, sizeof(new_file_path), "%s/%s", dir_path, STORE_FILENAME);

    /* Idempotent: when the resolved path and limit are unchanged, do not touch
     * the filesystem again. This avoids hammering a shared SPI SD bus every
     * time the HTTP API re-syncs the store path. */
    if (s_store.ready && s_store.max_bytes == config->max_file_bytes &&
            strcmp(s_store.file_path, new_file_path) == 0) {
        xSemaphoreGive(s_store.lock);
        return ESP_OK;
    }

    bool bus = store_bus_lock();

    esp_err_t err = ensure_directory(dir_path);
    if (err != ESP_OK) {
        store_bus_unlock(bus);
        xSemaphoreGive(s_store.lock);
        return err;
    }

    strlcpy(s_store.file_path, new_file_path, sizeof(s_store.file_path));
    s_store.max_bytes = config->max_file_bytes;

    FILE *fp = fopen(s_store.file_path, "a");
    if (!fp) {
        store_bus_unlock(bus);
        xSemaphoreGive(s_store.lock);
        return ESP_FAIL;
    }
    fclose(fp);

    s_store.ready = true;

    update_cached_stats();
    store_bus_unlock(bus);
    ESP_LOGI(TAG, "init: path=%s max=%u count=%u size=%u",
             s_store.file_path, (unsigned)s_store.max_bytes,
             (unsigned)s_store.msg_count, (unsigned)s_store.file_size);

    xSemaphoreGive(s_store.lock);
    return ESP_OK;
}

esp_err_t meshtastic_store_append(uint32_t from, const char *from_id,
                                  uint32_t channel, uint32_t packet_id,
                                  const char *text, int64_t ts_ms)
{
    if (!s_store.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_store.lock, portMAX_DELAY);

    /* Build one JSONL line using cJSON for proper escaping (no I/O here). */
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(obj, "from", from_id ? from_id : "?");
    cJSON_AddNumberToObject(obj, "from_num", (double)from);
    cJSON_AddNumberToObject(obj, "channel", (double)channel);
    cJSON_AddNumberToObject(obj, "packet_id", (double)packet_id);
    cJSON_AddStringToObject(obj, "text", text ? text : "");
    cJSON_AddNumberToObject(obj, "ts", (double)ts_ms);

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!line) {
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_NO_MEM;
    }

    bool bus = store_bus_lock();

    FILE *fp = fopen(s_store.file_path, "a");
    if (!fp) {
        store_bus_unlock(bus);
        free(line);
        xSemaphoreGive(s_store.lock);
        ESP_LOGW(TAG, "append: open failed");
        return ESP_FAIL;
    }

    int written = fprintf(fp, "%s\n", line);
    free(line);
    fclose(fp);

    if (written > 0) {
        s_store.file_size += (size_t)written;
        s_store.msg_count++;
    }

    rotate_if_needed();

    store_bus_unlock(bus);
    xSemaphoreGive(s_store.lock);
    return ESP_OK;
}

esp_err_t meshtastic_store_read(cJSON *array, size_t max_count)
{
    if (!array) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_store.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_store.lock, portMAX_DELAY);

    bool bus = store_bus_lock();

    FILE *fp = fopen(s_store.file_path, "r");
    if (!fp) {
        store_bus_unlock(bus);
        xSemaphoreGive(s_store.lock);
        return ESP_OK; /* empty store, not an error */
    }

    /*
     * Collect all lines first, then reverse-insert into the array so
     * the newest message appears first.
     */
    cJSON **items = NULL;
    size_t count = 0;
    size_t capacity = 64;

    items = calloc(capacity, sizeof(cJSON *));
    if (!items) {
        fclose(fp);
        store_bus_unlock(bus);
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_NO_MEM;
    }

    char *line = NULL;
    while ((line = read_line_alloc(fp, NULL)) != NULL) {
        if (line[0] != '{') {
            free(line);
            continue;
        }

        cJSON *obj = cJSON_Parse(line);
        free(line);
        if (!obj) {
            continue;
        }

        if (count >= capacity) {
            size_t new_cap = capacity * 2;
            cJSON **tmp = realloc(items, new_cap * sizeof(cJSON *));
            if (!tmp) {
                cJSON_Delete(obj);
                break;
            }
            items = tmp;
            capacity = new_cap;
        }
        items[count++] = obj;
    }
    fclose(fp);
    store_bus_unlock(bus);

    /* Insert newest-first, respecting max_count. */
    size_t start = 0;
    if (max_count > 0 && count > max_count) {
        start = count - max_count;
    }
    for (size_t i = count; i > start; i--) {
        cJSON_AddItemToArray(array, items[i - 1]);
        items[i - 1] = NULL; /* ownership transferred */
    }

    /* Free any remaining items (if max_count limited them). */
    for (size_t i = 0; i < start; i++) {
        cJSON_Delete(items[i]);
    }
    free(items);

    xSemaphoreGive(s_store.lock);
    return ESP_OK;
}

esp_err_t meshtastic_store_clear(void)
{
    if (!s_store.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_store.lock, portMAX_DELAY);
    bool bus = store_bus_lock();
    remove(s_store.file_path);
    store_bus_unlock(bus);
    s_store.file_size = 0;
    s_store.msg_count = 0;
    ESP_LOGI(TAG, "store cleared");
    xSemaphoreGive(s_store.lock);
    return ESP_OK;
}

size_t meshtastic_store_count(void)
{
    return s_store.msg_count;
}

size_t meshtastic_store_file_size(void)
{
    return s_store.file_size;
}

const char *meshtastic_store_path(void)
{
    return s_store.ready ? s_store.file_path : NULL;
}

bool meshtastic_store_is_ready(void)
{
    return s_store.ready;
}
