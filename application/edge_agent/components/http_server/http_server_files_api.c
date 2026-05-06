/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spi_bus_arbiter.h"

static const char *FILES_API_TAG = "http_files";

static int mkdir_parents(char *path, mode_t mode)
{
    if (!path || path[0] != '/') {
        return -1;
    }
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(path, mode) != 0 && errno != EEXIST) {
            *p = '/';
            return -1;
        }
        *p = '/';
    }
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static esp_err_t files_list_handler(httpd_req_t *req)
{
    char relative_path[HTTP_SERVER_PATH_MAX] = "/";
    if (http_server_query_get(req, "path", relative_path, sizeof(relative_path)) != ESP_OK) {
        strlcpy(relative_path, "/", sizeof(relative_path));
    }

    /* If the request targets a virtual mount whose backing storage has gone
     * away (e.g. SD card pulled), report a clean error instead of letting
     * opendir() spin against the dead device. The slot is kept registered
     * so future refreshes can re-probe (e.g. card re-inserted). */
    const http_server_extra_mount_t *target_mount =
        http_server_match_extra_mount(relative_path);
    if (target_mount && target_mount->alive_cb && !target_mount->alive_cb()) {
        ESP_LOGW(FILES_API_TAG, "virtual mount %s not currently available",
                 target_mount->vroot);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "Mount not available");
    }

    char full_path[HTTP_SERVER_PATH_MAX];
    if (http_server_resolve_storage_path(relative_path, full_path, sizeof(full_path)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    DIR *dir = opendir(full_path);
    if (!dir) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory not found");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    if (!root || !entries) {
        closedir(dir);
        cJSON_Delete(root);
        cJSON_Delete(entries);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    http_server_json_add_string(root, "path", relative_path);
    cJSON_AddItemToObject(root, "entries", entries);

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_relative[HTTP_SERVER_PATH_MAX];
        char child_full[HTTP_SERVER_PATH_MAX];
        if (!http_server_build_child_relative_path(relative_path, entry->d_name, child_relative, sizeof(child_relative)) ||
            http_server_resolve_storage_path(child_relative, child_full, sizeof(child_full)) != ESP_OK) {
            continue;
        }

        struct stat st = {0};
        if (stat(child_full, &st) != 0) {
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }

        http_server_json_add_string(item, "name", entry->d_name);
        http_server_json_add_string(item, "path", child_relative);
        cJSON_AddBoolToObject(item, "is_dir", S_ISDIR(st.st_mode));
        cJSON_AddNumberToObject(item, "size", (double)st.st_size);
        cJSON_AddItemToArray(entries, item);
    }

    closedir(dir);

    /* When listing the default root, inject one synthetic entry per
     * registered virtual mount (e.g. an "/sdcard" SD-card link).
     * Slots whose alive_cb returns false are simply hidden — the slot
     * itself is kept so a future refresh can revive the mount. */
    if (strcmp(relative_path, "/") == 0) {
        http_server_ctx_t *ctx = http_server_ctx();
        int injected = 0;
        for (int i = 0; i < HTTP_SERVER_MAX_EXTRA_MOUNTS; ++i) {
            const http_server_extra_mount_t *m = &ctx->extra_mounts[i];
            if (m->vroot[0] == '\0') {
                continue;
            }
            if (m->alive_cb && !m->alive_cb()) {
                ESP_LOGD(FILES_API_TAG, "hiding inactive virtual mount %s",
                         m->vroot);
                continue;
            }
            const char *name = m->vroot[0] == '/' ? m->vroot + 1 : m->vroot;
            cJSON *item = cJSON_CreateObject();
            if (!item) {
                continue;
            }
            http_server_json_add_string(item, "name", name);
            http_server_json_add_string(item, "path", m->vroot);
            cJSON_AddBoolToObject(item, "is_dir", true);
            cJSON_AddNumberToObject(item, "size", 0);
            if (m->label[0]) {
                http_server_json_add_string(item, "mount", m->label);
            } else {
                http_server_json_add_string(item, "mount", "ext");
            }
            cJSON_AddItemToArray(entries, item);
            injected++;
        }
        ESP_LOGI(FILES_API_TAG, "list / : injected %d extra-mount entries", injected);
    }

    return http_server_send_json_response(req, root);
}

static esp_err_t file_download_handler(httpd_req_t *req)
{
    const char *relative_path = req->uri + strlen("/files");
    if (!http_server_path_is_safe(relative_path)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    char full_path[HTTP_SERVER_PATH_MAX];
    if (http_server_resolve_storage_path(relative_path, full_path, sizeof(full_path)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    struct stat st = {0};
    if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }

    FILE *file = fopen(full_path, "rb");
    if (!file) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
    }

    char *scratch = http_server_alloc_scratch_buffer();
    if (!scratch) {
        fclose(file);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    bool needs_spi_lock = http_server_match_extra_mount(relative_path) != NULL;
    while (!feof(file)) {
        bool spi_locked = false;
        if (needs_spi_lock) {
            spi_locked = (spi_bus_arbiter_lock(2000) == ESP_OK);
        }
        size_t read_bytes = fread(scratch, 1, HTTP_SERVER_SCRATCH_SIZE, file);
        if (spi_locked) {
            spi_bus_arbiter_unlock();
        }
        if (read_bytes > 0 && httpd_resp_send_chunk(req, scratch, read_bytes) != ESP_OK) {
            free(scratch);
            fclose(file);
            return ESP_FAIL;
        }
    }

    free(scratch);
    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t files_upload_handler(httpd_req_t *req)
{
    char relative_path[HTTP_SERVER_PATH_MAX] = {0};
    if (http_server_query_get(req, "path", relative_path, sizeof(relative_path)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid upload size");
    }
    /* Allow much larger uploads when the destination is a registered virtual
     * mount (SD card etc.); cap stays small for the on-chip FATFS partition. */
    size_t max_size = http_server_match_extra_mount(relative_path)
                      ? HTTP_SERVER_UPLOAD_MAX_SIZE_SD
                      : HTTP_SERVER_UPLOAD_MAX_SIZE;
    if ((size_t)req->content_len > max_size) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Upload too large (max %u bytes)",
                 (unsigned)max_size);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }

    char full_path[HTTP_SERVER_PATH_MAX];
    if (http_server_resolve_storage_path(relative_path, full_path, sizeof(full_path)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    char parent_path[HTTP_SERVER_PATH_MAX];
    strlcpy(parent_path, full_path, sizeof(parent_path));
    char *slash = strrchr(parent_path, '/');
    if (!slash || slash == parent_path) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }
    *slash = '\0';

    struct stat st = {0};
    if (stat(parent_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Parent directory not found");
    }

    FILE *file = fopen(full_path, "wb");
    if (!file) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
    }

    char *scratch = http_server_alloc_scratch_buffer();
    if (!scratch) {
        fclose(file);
        unlink(full_path);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    /* If the upload targets a virtual mount (SD card on a shared SPI host),
     * take the SPI bus arbiter around each fwrite() so the LCD/touch drivers
     * on the same host cannot interleave with SDSPI polling and trigger
     * the spi_hal_setup_trans assert. We lock per-chunk (not for the whole
     * upload) so display refresh is not blocked for the whole transfer. */
    bool needs_spi_lock = http_server_match_extra_mount(relative_path) != NULL;

    int remaining = req->content_len;
    while (remaining > 0) {
        int chunk = remaining > HTTP_SERVER_SCRATCH_SIZE ? HTTP_SERVER_SCRATCH_SIZE : remaining;
        int received = httpd_req_recv(req, scratch, chunk);
        if (received <= 0) {
            free(scratch);
            fclose(file);
            unlink(full_path);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
        }
        bool spi_locked = false;
        if (needs_spi_lock) {
            spi_locked = (spi_bus_arbiter_lock(2000) == ESP_OK);
        }
        size_t written = fwrite(scratch, 1, received, file);
        if (spi_locked) {
            spi_bus_arbiter_unlock();
        }
        if (written != (size_t)received) {
            free(scratch);
            fclose(file);
            unlink(full_path);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
        }
        /* Yield so the IDLE task on this single-core target can run and
         * reset the watchdog. SDSPI writes at low frequency can keep the
         * httpd task hot for many seconds otherwise. */
        vTaskDelay(1);
        remaining -= received;
    }

    free(scratch);
    bool spi_locked = false;
    if (needs_spi_lock) {
        spi_locked = (spi_bus_arbiter_lock(2000) == ESP_OK);
    }
    fclose(file);   /* may issue final SDSPI writes for the BPB / FAT cache */
    if (spi_locked) {
        spi_bus_arbiter_unlock();
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t files_delete_handler(httpd_req_t *req)
{
    char relative_path[HTTP_SERVER_PATH_MAX] = {0};
    if (http_server_query_get(req, "path", relative_path, sizeof(relative_path)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
    }

    char full_path[HTTP_SERVER_PATH_MAX];
    if (http_server_resolve_storage_path(relative_path, full_path, sizeof(full_path)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    struct stat st = {0};
    if (stat(full_path, &st) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Path not found");
    }

    int rc = S_ISDIR(st.st_mode) ? rmdir(full_path) : unlink(full_path);
    if (rc != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t files_mkdir_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (!cJSON_IsString(path_item) || !http_server_path_is_safe(path_item->valuestring)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    cJSON *rec_item = cJSON_GetObjectItemCaseSensitive(root, "recursive");
    const bool mk_recursive = cJSON_IsBool(rec_item) && cJSON_IsTrue(rec_item);

    char full_path[HTTP_SERVER_PATH_MAX];
    esp_err_t err = http_server_resolve_storage_path(path_item->valuestring, full_path, sizeof(full_path));
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    if (mk_recursive) {
        char mkdir_buf[HTTP_SERVER_PATH_MAX];
        strlcpy(mkdir_buf, full_path, sizeof(mkdir_buf));
        if (mkdir_parents(mkdir_buf, 0775) != 0) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create directory");
        }
    } else if (mkdir(full_path, 0775) != 0 && errno != EEXIST) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create directory");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t http_server_register_files_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/files", .method = HTTP_GET, .handler = files_list_handler },
        { .uri = "/api/files", .method = HTTP_DELETE, .handler = files_delete_handler },
        { .uri = "/api/files/upload", .method = HTTP_POST, .handler = files_upload_handler },
        { .uri = "/api/files/mkdir", .method = HTTP_POST, .handler = files_mkdir_handler },
        { .uri = "/files/*", .method = HTTP_GET, .handler = file_download_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
