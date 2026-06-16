/*
 * SPDX-FileCopyrightText: 2026 Chengdu RockBase Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "http_mesh";

static esp_err_t mesh_send_503(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

/* ── GET /api/mesh/messages?limit=100 ────────────────────────────────── */

static esp_err_t mesh_messages_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();

    if (!ctx->services.get_mesh_messages) {
        return mesh_send_503(req, "Meshtastic not configured");
    }

    /* Parse optional ?limit= query parameter. */
    size_t limit = 200;
    char limit_buf[16] = {0};
    if (http_server_query_get(req, "limit", limit_buf, sizeof(limit_buf)) == ESP_OK &&
            limit_buf[0]) {
        int v = atoi(limit_buf);
        if (v > 0 && v < 10000) {
            limit = (size_t)v;
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON *arr = cJSON_AddArrayToObject(root, "messages");
    esp_err_t err = ctx->services.get_mesh_messages(arr, limit);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to read messages");
    }

    cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(arr));
    return http_server_send_json_response(req, root);
}

/* ── DELETE /api/mesh/messages ───────────────────────────────────────── */

static esp_err_t mesh_clear_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();

    if (!ctx->services.clear_mesh_messages) {
        return mesh_send_503(req, "Meshtastic not configured");
    }

    esp_err_t err = ctx->services.clear_mesh_messages();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to clear messages");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "messages cleared");
    ESP_LOGI(TAG, "mesh messages cleared via HTTP API");
    return http_server_send_json_response(req, root);
}

/* ── GET /api/mesh/status ────────────────────────────────────────────── */

static esp_err_t mesh_status_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();

    if (!ctx->services.get_mesh_status) {
        return mesh_send_503(req, "Meshtastic not configured");
    }

    http_server_mesh_status_t status = {0};
    esp_err_t err = ctx->services.get_mesh_status(&status);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to get mesh status");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "connected", status.connected);
    cJSON_AddNumberToObject(root, "message_count", (double)status.count);
    cJSON_AddNumberToObject(root, "file_size_bytes", (double)status.file_size_bytes);
    if (status.store_path) {
        cJSON_AddStringToObject(root, "store_path", status.store_path);
    }

    return http_server_send_json_response(req, root);
}

/* ── GET /api/mesh/im ─────────────────────────────────────────────────
 * List the IM channels inbound mesh messages can be pushed to, including
 * whether each is available (configured), enabled and has a known target. */
static esp_err_t mesh_im_get_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();

    if (!ctx->services.get_mesh_im_targets) {
        return mesh_send_503(req, "Meshtastic not configured");
    }

    http_server_mesh_im_t targets[8] = {0};
    size_t count = 0;
    esp_err_t err = ctx->services.get_mesh_im_targets(targets, 8, &count);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to get IM push targets");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "channels");
    for (size_t i = 0; arr && i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        cJSON_AddStringToObject(obj, "channel", targets[i].channel);
        cJSON_AddBoolToObject(obj, "enabled", targets[i].enabled);
        cJSON_AddBoolToObject(obj, "has_target", targets[i].has_target);
        cJSON_AddItemToArray(arr, obj);
    }
    return http_server_send_json_response(req, root);
}

/* ── POST /api/mesh/im  body: {channel, enabled} ──────────────────────── */
static esp_err_t mesh_im_set_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();

    if (!ctx->services.set_mesh_im_target) {
        return mesh_send_503(req, "Meshtastic not configured");
    }

    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK || !root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    char channel[16] = {0};
    http_server_json_read_string(root, "channel", channel, sizeof(channel));
    cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (!channel[0] || !cJSON_IsBool(en)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Missing 'channel' or 'enabled'");
    }
    bool enabled = cJSON_IsTrue(en);
    cJSON_Delete(root);

    esp_err_t err = ctx->services.set_mesh_im_target(channel, enabled);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Unknown or unavailable IM channel");
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "channel", channel);
    cJSON_AddBoolToObject(resp, "enabled", enabled);
    return http_server_send_json_response(req, resp);
}

/* ── Route registration ─────────────────────────────────────────────── */

esp_err_t http_server_register_mesh_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/mesh/messages", .method = HTTP_GET,    .handler = mesh_messages_handler },
        { .uri = "/api/mesh/messages", .method = HTTP_DELETE, .handler = mesh_clear_handler },
        { .uri = "/api/mesh/status",   .method = HTTP_GET,    .handler = mesh_status_handler },
        { .uri = "/api/mesh/im",       .method = HTTP_GET,    .handler = mesh_im_get_handler },
        { .uri = "/api/mesh/im",       .method = HTTP_POST,   .handler = mesh_im_set_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
