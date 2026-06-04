/*
 * SPDX-FileCopyrightText: 2026 RockBase-iot Technology (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "logo.h"

static const char *TAG = "http_logo";

/* Maximum SVG upload size (512 KiB). */
#define LOGO_UPLOAD_MAX_SIZE (512 * 1024)

/**
 * @brief  Ensure the parent directory of the logo SVG path exists.
 */
static esp_err_t logo_ensure_directory(void)
{
    const char *path = logo_get_path();
    char dir[256];
    strlcpy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return ESP_ERR_INVALID_ARG;
    }
    *slash = '\0';

    struct stat st;
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    if (mkdir(dir, 0775) != 0) {
        ESP_LOGE(TAG, "mkdir %s failed", dir);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * POST /api/logo  -- upload SVG body, save to FATFS, refresh display.
 */
static esp_err_t logo_upload_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || (size_t)req->content_len > LOGO_UPLOAD_MAX_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Invalid or too-large SVG upload");
    }

    esp_err_t dir_err = logo_ensure_directory();
    if (dir_err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create logo directory");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to create logo directory");
    }

    const char *path = logo_get_path();
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "failed to open %s for writing", path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to create logo file");
    }

    char *buf = http_server_alloc_scratch_buffer();
    if (!buf) {
        fclose(f);
        unlink(path);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int chunk = remaining > HTTP_SERVER_SCRATCH_SIZE ? HTTP_SERVER_SCRATCH_SIZE : remaining;
        int received = httpd_req_recv(req, buf, chunk);
        if (received <= 0) {
            free(buf);
            fclose(f);
            unlink(path);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Upload interrupted");
        }
        if (fwrite(buf, 1, received, f) != (size_t)received) {
            free(buf);
            fclose(f);
            unlink(path);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Write failed");
        }
        remaining -= received;
    }

    free(buf);
    fclose(f);

    ESP_LOGI(TAG, "logo uploaded (%u bytes)", (unsigned)req->content_len);

    /* Refresh the display with the new logo. */
    esp_err_t err = logo_refresh();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "logo_refresh failed: %s", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/**
 * GET /api/logo  -- return logo status JSON.
 */
static esp_err_t logo_status_handler(httpd_req_t *req)
{
    const char *path = logo_get_path();
    struct stat st = {0};
    bool exists = (stat(path, &st) == 0 && S_ISREG(st.st_mode));

    char json[192];
    snprintf(json, sizeof(json),
             "{\"active\":%s,\"exists\":%s,\"size\":%ld,\"path\":\"%s\"}",
             logo_is_active() ? "true" : "false",
             exists ? "true" : "false",
             exists ? (long)st.st_size : 0L,
             path);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, json);
}

/**
 * DELETE /api/logo  -- remove custom logo, restore emote idle.
 */
static esp_err_t logo_delete_handler(httpd_req_t *req)
{
    const char *path = logo_get_path();

    /* Stop displaying the logo and release display arbiter. */
    logo_stop();

    struct stat st;
    if (stat(path, &st) == 0) {
        unlink(path);
        ESP_LOGI(TAG, "logo deleted: %s", path);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/**
 * GET /logo.svg  -- serve the current SVG for browser preview.
 */
static esp_err_t logo_svg_handler(httpd_req_t *req)
{
    const char *path = logo_get_path();
    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No custom logo");
    }

    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    char *buf = http_server_alloc_scratch_buffer();
    if (!buf) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    while (!feof(f)) {
        size_t n = fread(buf, 1, HTTP_SERVER_SCRATCH_SIZE, f);
        if (n > 0 && httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            free(buf);
            fclose(f);
            return ESP_FAIL;
        }
    }

    free(buf);
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t http_server_register_logo_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/logo", .method = HTTP_POST,   .handler = logo_upload_handler },
        { .uri = "/api/logo", .method = HTTP_GET,    .handler = logo_status_handler },
        { .uri = "/api/logo", .method = HTTP_DELETE,  .handler = logo_delete_handler },
        { .uri = "/logo.svg", .method = HTTP_GET,     .handler = logo_svg_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
