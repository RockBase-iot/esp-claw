/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "http_status";

/* ── Helpers ────────────────────────────────────────────────────────── */

/**
 * @brief  Append a storage-mount descriptor to `root`.
 *
 * Schema (all sizes in bytes):
 *   { "mount": "/fatfs", "total": 3072000, "free": 2048000, "mounted": true }
 */
static void add_storage_entry(cJSON *parent, const char *key,
                              const char *mount_path, bool mounted,
                              uint64_t total, uint64_t free_bytes)
{
    cJSON *entry = cJSON_CreateObject();
    if (!entry) {
        return;
    }
    http_server_json_add_string(entry, "mount", mount_path);
    cJSON_AddBoolToObject(entry, "mounted", mounted);
    /* Use cJSON_AddNumberToObject (double) — safe up to 2^53 bytes (9 PB). */
    cJSON_AddNumberToObject(entry, "total", (double)total);
    cJSON_AddNumberToObject(entry, "free", (double)free_bytes);
    cJSON_AddItemToObject(parent, key, entry);
}

/* ── Handlers ───────────────────────────────────────────────────────── */

static esp_err_t status_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wifi_status_t status = {0};
    esp_err_t err = ctx->services.get_wifi_status(&status);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read Wi-Fi status");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "wifi_connected", status.wifi_connected);
    http_server_json_add_string(root, "ip", status.ip);
    http_server_json_add_string(root, "storage_base_path", ctx->storage_base_path);
    cJSON_AddBoolToObject(root, "ap_active", status.ap_active);
    http_server_json_add_string(root, "ap_ssid", status.ap_ssid);
    http_server_json_add_string(root, "ap_ip", status.ap_ip);
    http_server_json_add_string(root, "wifi_mode", status.wifi_mode);

    /* ── Storage mounts ────────────────────────────────────────────── */
    {
        cJSON *storage = cJSON_CreateObject();
        if (storage) {
            /* FATFS (internal SPI flash partition) */
            http_server_storage_info_t fat_info = {0};
            if (ctx->services.get_storage_info &&
                    ctx->services.get_storage_info("fatfs", &fat_info) == ESP_OK) {
                add_storage_entry(storage, "fatfs", fat_info.mount_path,
                                  fat_info.mounted, fat_info.total_bytes,
                                  fat_info.free_bytes);
            } else {
                /* Fallback: query FATFS directly */
                uint64_t fat_total = 0, fat_free = 0;
                esp_err_t fat_err = esp_vfs_fat_info(ctx->storage_base_path,
                                                     &fat_total, &fat_free);
                bool fat_mounted = (fat_err == ESP_OK);
                add_storage_entry(storage, "fatfs", ctx->storage_base_path,
                                  fat_mounted, fat_total, fat_free);
                if (fat_err != ESP_OK) {
                    ESP_LOGW(TAG, "FATFS info query failed: %s",
                             esp_err_to_name(fat_err));
                }
            }

            /* SD card (optional — queried via service callback) */
            http_server_storage_info_t sd_info = {0};
            if (ctx->services.get_storage_info &&
                    ctx->services.get_storage_info("sdcard", &sd_info) == ESP_OK &&
                    sd_info.mount_path) {
                add_storage_entry(storage, "sdcard", sd_info.mount_path,
                                  sd_info.mounted, sd_info.total_bytes,
                                  sd_info.free_bytes);
            }

            cJSON_AddItemToObject(root, "storage", storage);
        }
    }

    return http_server_send_json_response(req, root);
}

static esp_err_t probe_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wifi_status_t status = {0};

    if (ctx->services.get_wifi_status) {
        (void)ctx->services.get_wifi_status(&status);
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "device", "esp-claw");
    cJSON_AddBoolToObject(root, "wifi_connected", status.wifi_connected);
    http_server_json_add_string(root, "ip", status.ip);
    cJSON_AddBoolToObject(root, "ap_active", status.ap_active);
    http_server_json_add_string(root, "ap_ip", status.ap_ip);
    http_server_json_add_string(root, "wifi_mode", status.wifi_mode);
    return http_server_send_json_response(req, root);
}

static esp_err_t alive_handler(httpd_req_t *req)
{
    return probe_handler(req);
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    esp_err_t err = ctx->services.restart_device ? ctx->services.restart_device() : ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to restart device");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "message", "device restart scheduled");
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_status_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/probe", .method = HTTP_GET, .handler = probe_handler },
        { .uri = "/alive", .method = HTTP_GET, .handler = alive_handler },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
