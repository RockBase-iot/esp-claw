/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "config_http_server.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "basic_demo_settings.h"
#include "basic_demo_wifi.h"
#include "cap_im_wechat.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spi_bus_arbiter.h"

static const char *TAG = "config_http";

#define CONFIG_HTTP_CTRL_PORT         32769
#define CONFIG_HTTP_SCRATCH_SIZE      4096
#define CONFIG_HTTP_PATH_MAX          256
#define CONFIG_HTTP_UPLOAD_MAX_SIZE   (512 * 1024)
/* Larger cap for uploads targeting a registered virtual mount (SD card). */
#define CONFIG_HTTP_UPLOAD_MAX_SIZE_SD (64 * 1024 * 1024)

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t styles_css_start[] asm("_binary_styles_css_start");
extern const uint8_t styles_css_end[] asm("_binary_styles_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t lean_qr_min_mjs_start[] asm("_binary_lean_qr_min_mjs_start");
extern const uint8_t lean_qr_min_mjs_end[] asm("_binary_lean_qr_min_mjs_end");

typedef bool (*config_http_server_mount_alive_cb_t)(void);

typedef struct {
    char vroot[64];
    char real_path[64];
    char label[16];
    config_http_server_mount_alive_cb_t alive_cb;
} config_http_server_extra_mount_t;

#define CONFIG_HTTP_MAX_EXTRA_MOUNTS 4

typedef struct {
    httpd_handle_t server;
    char storage_base_path[CONFIG_HTTP_PATH_MAX];
    config_http_server_extra_mount_t extra_mounts[CONFIG_HTTP_MAX_EXTRA_MOUNTS];
} config_http_server_ctx_t;

static config_http_server_ctx_t s_ctx = {0};

static const config_http_server_extra_mount_t *match_extra_mount(const char *relative_path)
{
    if (!relative_path || relative_path[0] != '/') {
        return NULL;
    }
    for (int i = 0; i < CONFIG_HTTP_MAX_EXTRA_MOUNTS; ++i) {
        const config_http_server_extra_mount_t *m = &s_ctx.extra_mounts[i];
        if (m->vroot[0] == '\0') {
            continue;
        }
        size_t vlen = strlen(m->vroot);
        if (strncmp(relative_path, m->vroot, vlen) != 0) {
            continue;
        }
        char next = relative_path[vlen];
        if (next == '\0' || next == '/') {
            return m;
        }
    }
    return NULL;
}

static char *alloc_scratch_buffer(void)
{
    return heap_caps_malloc_prefer(CONFIG_HTTP_SCRATCH_SIZE,
                                   2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static bool path_is_safe(const char *path)
{
    return path && path[0] == '/' && strstr(path, "..") == NULL;
}

static void url_decode_inplace(char *value)
{
    if (!value) {
        return;
    }

    char *src = value;
    char *dst = value;
    while (*src) {
        if (src[0] == '%' && src[1] && src[2]) {
            char hi = src[1];
            char lo = src[2];
            uint8_t decoded = 0;

            if (hi >= '0' && hi <= '9') {
                decoded = (uint8_t)(hi - '0') << 4;
            } else if (hi >= 'A' && hi <= 'F') {
                decoded = (uint8_t)(hi - 'A' + 10) << 4;
            } else if (hi >= 'a' && hi <= 'f') {
                decoded = (uint8_t)(hi - 'a' + 10) << 4;
            } else {
                *dst++ = *src++;
                continue;
            }

            if (lo >= '0' && lo <= '9') {
                decoded |= (uint8_t)(lo - '0');
            } else if (lo >= 'A' && lo <= 'F') {
                decoded |= (uint8_t)(lo - 'A' + 10);
            } else if (lo >= 'a' && lo <= 'f') {
                decoded |= (uint8_t)(lo - 'a' + 10);
            } else {
                *dst++ = *src++;
                continue;
            }

            *dst++ = (char)decoded;
            src += 3;
            continue;
        }

        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
}

static esp_err_t query_get(httpd_req_t *req, const char *key, char *value, size_t value_size)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char *query = calloc(1, query_len + 1);
    if (!query) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = httpd_req_get_url_query_str(req, query, query_len + 1);
    if (err == ESP_OK) {
        err = httpd_query_key_value(query, key, value, value_size);
        if (err == ESP_OK) {
            url_decode_inplace(value);
        }
    }

    free(query);
    return err;
}

static esp_err_t send_embedded_file(httpd_req_t *req,
                                    const uint8_t *start,
                                    const uint8_t *end,
                                    const char *content_type)
{
    size_t content_len = (size_t)(end - start);
    if (content_len > 0 && start[content_len - 1] == '\0') {
        content_len--;
    }
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, (const char *)start, content_len);
}

static void json_add_string(cJSON *root, const char *key, const char *value)
{
    cJSON_AddStringToObject(root, key, value ? value : "");
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *root)
{
    char *payload = NULL;
    esp_err_t err;

    if (!req || !root) {
        return ESP_ERR_INVALID_ARG;
    }

    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    err = httpd_resp_sendstr(req, payload);
    free(payload);
    return err;
}

static esp_err_t settings_to_json(httpd_req_t *req)
{
    basic_demo_settings_t settings;
    ESP_RETURN_ON_ERROR(basic_demo_settings_load(&settings), TAG, "Failed to load settings");

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    json_add_string(root, "wifi_ssid", settings.wifi_ssid);
    json_add_string(root, "wifi_password", settings.wifi_password);
    json_add_string(root, "llm_api_key", settings.llm_api_key);
    json_add_string(root, "llm_backend_type", settings.llm_backend_type);
    json_add_string(root, "llm_profile", settings.llm_profile);
    json_add_string(root, "llm_model", settings.llm_model);
    json_add_string(root, "llm_base_url", settings.llm_base_url);
    json_add_string(root, "llm_auth_type", settings.llm_auth_type);
    json_add_string(root, "llm_timeout_ms", settings.llm_timeout_ms);
    json_add_string(root, "qq_app_id", settings.qq_app_id);
    json_add_string(root, "qq_app_secret", settings.qq_app_secret);
    json_add_string(root, "feishu_app_id", settings.feishu_app_id);
    json_add_string(root, "feishu_app_secret", settings.feishu_app_secret);
    json_add_string(root, "tg_bot_token", settings.tg_bot_token);
    json_add_string(root, "wechat_token", settings.wechat_token);
    json_add_string(root, "wechat_base_url", settings.wechat_base_url);
    json_add_string(root, "wechat_cdn_base_url", settings.wechat_cdn_base_url);
    json_add_string(root, "wechat_account_id", settings.wechat_account_id);
    json_add_string(root, "search_brave_key", settings.search_brave_key);
    json_add_string(root, "search_tavily_key", settings.search_tavily_key);
    json_add_string(root, "time_timezone", settings.time_timezone);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);
    return err;
}

static esp_err_t parse_json_body(httpd_req_t *req, cJSON **out_root)
{
    if (!out_root || req->content_len <= 0 || req->content_len > 8192) {
        return ESP_ERR_INVALID_ARG;
    }

    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return (ret == HTTPD_SOCK_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        received += ret;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_root = root;
    return ESP_OK;
}

static void json_read_string(cJSON *root, const char *key, char *buffer, size_t buffer_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item)) {
        strlcpy(buffer, item->valuestring, buffer_size);
    }
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return send_embedded_file(req, index_html_start, index_html_end, "text/html; charset=utf-8");
}

static esp_err_t styles_handler(httpd_req_t *req)
{
    return send_embedded_file(req, styles_css_start, styles_css_end, "text/css; charset=utf-8");
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    return send_embedded_file(req, app_js_start, app_js_end, "application/javascript; charset=utf-8");
}

static esp_err_t lean_qr_handler(httpd_req_t *req)
{
    return send_embedded_file(req,
                              lean_qr_min_mjs_start,
                              lean_qr_min_mjs_end,
                              "application/javascript; charset=utf-8");
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "wifi_connected", basic_demo_wifi_is_connected());
    json_add_string(root, "ip", basic_demo_wifi_get_ip());
    json_add_string(root, "storage_base_path", s_ctx.storage_base_path);
    cJSON_AddBoolToObject(root, "ap_active", basic_demo_wifi_is_ap_active());
    json_add_string(root, "ap_ssid", basic_demo_wifi_get_ap_ssid());
    json_add_string(root, "ap_ip", basic_demo_wifi_get_ap_ip());
    json_add_string(root, "wifi_mode", basic_demo_wifi_get_mode_string());

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);
    return err;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    return settings_to_json(req);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    basic_demo_settings_t settings;
    ESP_RETURN_ON_ERROR(basic_demo_settings_load(&settings), TAG, "Failed to load settings");

    cJSON *root = NULL;
    esp_err_t err = parse_json_body(req, &root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
        return err;
    }

    json_read_string(root, "wifi_ssid", settings.wifi_ssid, sizeof(settings.wifi_ssid));
    json_read_string(root, "wifi_password", settings.wifi_password, sizeof(settings.wifi_password));
    json_read_string(root, "llm_api_key", settings.llm_api_key, sizeof(settings.llm_api_key));
    json_read_string(root, "llm_backend_type", settings.llm_backend_type, sizeof(settings.llm_backend_type));
    json_read_string(root, "llm_profile", settings.llm_profile, sizeof(settings.llm_profile));
    json_read_string(root, "llm_model", settings.llm_model, sizeof(settings.llm_model));
    json_read_string(root, "llm_base_url", settings.llm_base_url, sizeof(settings.llm_base_url));
    json_read_string(root, "llm_auth_type", settings.llm_auth_type, sizeof(settings.llm_auth_type));
    json_read_string(root, "llm_timeout_ms", settings.llm_timeout_ms, sizeof(settings.llm_timeout_ms));
    json_read_string(root, "qq_app_id", settings.qq_app_id, sizeof(settings.qq_app_id));
    json_read_string(root, "qq_app_secret", settings.qq_app_secret, sizeof(settings.qq_app_secret));
    json_read_string(root, "feishu_app_id", settings.feishu_app_id, sizeof(settings.feishu_app_id));
    json_read_string(root, "feishu_app_secret", settings.feishu_app_secret, sizeof(settings.feishu_app_secret));
    json_read_string(root, "tg_bot_token", settings.tg_bot_token, sizeof(settings.tg_bot_token));
    json_read_string(root, "wechat_token", settings.wechat_token, sizeof(settings.wechat_token));
    json_read_string(root, "wechat_base_url", settings.wechat_base_url, sizeof(settings.wechat_base_url));
    json_read_string(root, "wechat_cdn_base_url", settings.wechat_cdn_base_url, sizeof(settings.wechat_cdn_base_url));
    json_read_string(root, "wechat_account_id", settings.wechat_account_id, sizeof(settings.wechat_account_id));
    json_read_string(root, "search_brave_key", settings.search_brave_key, sizeof(settings.search_brave_key));
    json_read_string(root, "search_tavily_key", settings.search_tavily_key, sizeof(settings.search_tavily_key));
    json_read_string(root, "time_timezone", settings.time_timezone, sizeof(settings.time_timezone));

    cJSON_Delete(root);

    err = basic_demo_settings_save(&settings);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
        return err;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Saved. Restart the device to apply Wi-Fi and core LLM changes.\"}");
}

static esp_err_t wechat_login_persist_if_needed(cap_im_wechat_qr_login_status_t *status)
{
    basic_demo_settings_t settings;
    esp_err_t err;

    if (!status || !status->completed || status->persisted || !status->token[0]) {
        return ESP_OK;
    }

    err = basic_demo_settings_load(&settings);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(settings.wechat_token, status->token, sizeof(settings.wechat_token));
    strlcpy(settings.wechat_base_url,
            status->base_url[0] ? status->base_url : settings.wechat_base_url,
            sizeof(settings.wechat_base_url));
    strlcpy(settings.wechat_account_id,
            status->account_id[0] ? status->account_id : settings.wechat_account_id,
            sizeof(settings.wechat_account_id));

    err = basic_demo_settings_save(&settings);
    if (err != ESP_OK) {
        return err;
    }

    return cap_im_wechat_qr_login_mark_persisted();
}

static esp_err_t wechat_login_start_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    cJSON *resp = NULL;
    const char *account_id = NULL;
    bool force = false;
    cap_im_wechat_qr_login_status_t status = {0};
    esp_err_t err;

    if (req->content_len > 0) {
        err = parse_json_body(req, &root);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
            return err;
        }
        account_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "account_id"));
        force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "force"));
    }

    err = cap_im_wechat_qr_login_start(account_id, force);
    if (root) {
        cJSON_Delete(root);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start WeChat login");
        return err;
    }

    ESP_RETURN_ON_ERROR(cap_im_wechat_qr_login_get_status(&status),
                        TAG,
                        "Failed to fetch WeChat login status");
    resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(resp, "ok", true);
    json_add_string(resp, "session_key", status.session_key);
    json_add_string(resp, "status", status.status);
    json_add_string(resp, "message", status.message);
    json_add_string(resp, "qr_data_url", status.qr_data_url);
    return send_json_response(req, resp);
}

static esp_err_t wechat_login_status_handler(httpd_req_t *req)
{
    cap_im_wechat_qr_login_status_t status = {0};
    cJSON *resp = NULL;
    esp_err_t err;

    err = cap_im_wechat_qr_login_get_status(&status);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read WeChat login status");
        return err;
    }

    err = wechat_login_persist_if_needed(&status);
    if (err == ESP_OK && status.completed && !status.persisted) {
        status.persisted = true;
        strlcpy(status.message,
                "微信登录成功，凭据已保存。重启设备后生效。",
                sizeof(status.message));
    }

    resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "active", status.active);
    cJSON_AddBoolToObject(resp, "completed", status.completed);
    cJSON_AddBoolToObject(resp, "persisted", status.persisted);
    cJSON_AddBoolToObject(resp, "configured", status.configured);
    json_add_string(resp, "session_key", status.session_key);
    json_add_string(resp, "status", status.status);
    json_add_string(resp, "message", status.message);
    json_add_string(resp, "qr_data_url", status.qr_data_url);
    json_add_string(resp, "account_id", status.account_id);
    json_add_string(resp, "user_id", status.user_id);
    json_add_string(resp, "base_url", status.base_url);
    cJSON_AddBoolToObject(resp, "restart_required", status.persisted);
    return send_json_response(req, resp);
}

static esp_err_t wechat_login_cancel_handler(httpd_req_t *req)
{
    cJSON *resp = NULL;
    esp_err_t err = cap_im_wechat_qr_login_cancel();

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to cancel WeChat login");
        return err;
    }

    resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    json_add_string(resp, "message", "已取消微信登录。");
    return send_json_response(req, resp);
}

static esp_err_t resolve_storage_path(const char *relative_path, char *full_path, size_t full_path_size)
{
    if (!path_is_safe(relative_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    const config_http_server_extra_mount_t *m = match_extra_mount(relative_path);
    if (m) {
        const char *tail = relative_path + strlen(m->vroot);
        int written = snprintf(full_path, full_path_size, "%s%s",
                               m->real_path, tail);
        if (written <= 0 || (size_t)written >= full_path_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    int written = snprintf(full_path, full_path_size, "%s%s", s_ctx.storage_base_path, relative_path);
    if (written <= 0 || (size_t)written >= full_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static bool build_child_relative_path(const char *base_path,
                                      const char *entry_name,
                                      char *out_path,
                                      size_t out_path_size)
{
    if (!base_path || !entry_name || !out_path || out_path_size == 0) {
        return false;
    }

    if (strcmp(base_path, "/") == 0) {
        if (strlcpy(out_path, "/", out_path_size) >= out_path_size) {
            return false;
        }
    } else if (strlcpy(out_path, base_path, out_path_size) >= out_path_size) {
        return false;
    }

    if (strcmp(base_path, "/") != 0 && strlcat(out_path, "/", out_path_size) >= out_path_size) {
        return false;
    }

    return strlcat(out_path, entry_name, out_path_size) < out_path_size;
}

static esp_err_t files_list_handler(httpd_req_t *req)
{
    char relative_path[CONFIG_HTTP_PATH_MAX] = "/";
    if (query_get(req, "path", relative_path, sizeof(relative_path)) != ESP_OK) {
        strlcpy(relative_path, "/", sizeof(relative_path));
    }

    /* If the request targets a virtual mount whose backing storage has gone
     * away (e.g. SD card pulled), report a clean error instead of letting
     * opendir() spin against the dead device. The slot is kept registered
     * so future refreshes can re-probe (e.g. card re-inserted). */
    const config_http_server_extra_mount_t *target_mount = match_extra_mount(relative_path);
    if (target_mount && target_mount->alive_cb && !target_mount->alive_cb()) {
        ESP_LOGW(TAG, "virtual mount %s not currently available", target_mount->vroot);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Mount not available");
        return ESP_FAIL;
    }

    char full_path[CONFIG_HTTP_PATH_MAX];
    if (resolve_storage_path(relative_path, full_path, sizeof(full_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_ERR_INVALID_ARG;
    }

    DIR *dir = opendir(full_path);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory not found");
        return ESP_FAIL;
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

    json_add_string(root, "path", relative_path);
    cJSON_AddItemToObject(root, "entries", entries);

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_relative[CONFIG_HTTP_PATH_MAX];
        if (!build_child_relative_path(relative_path, entry->d_name, child_relative, sizeof(child_relative))) {
            continue;
        }

        char child_full[CONFIG_HTTP_PATH_MAX];
        if (resolve_storage_path(child_relative, child_full, sizeof(child_full)) != ESP_OK) {
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

        json_add_string(item, "name", entry->d_name);
        json_add_string(item, "path", child_relative);
        cJSON_AddBoolToObject(item, "is_dir", S_ISDIR(st.st_mode));
        cJSON_AddNumberToObject(item, "size", (double)st.st_size);
        cJSON_AddItemToArray(entries, item);
    }

    closedir(dir);

    /* When listing the default root, append a synthetic entry per
     * registered virtual mount (e.g. SD card). Slots whose alive_cb
     * returns false are simply hidden — the slot itself is kept so a
     * future refresh can revive the mount. */
    if (strcmp(relative_path, "/") == 0) {
        int injected = 0;
        for (int i = 0; i < CONFIG_HTTP_MAX_EXTRA_MOUNTS; ++i) {
            const config_http_server_extra_mount_t *m = &s_ctx.extra_mounts[i];
            if (m->vroot[0] == '\0') {
                continue;
            }
            if (m->alive_cb && !m->alive_cb()) {
                ESP_LOGD(TAG, "hiding inactive virtual mount %s", m->vroot);
                continue;
            }
            const char *name = m->vroot[0] == '/' ? m->vroot + 1 : m->vroot;
            cJSON *item = cJSON_CreateObject();
            if (!item) {
                continue;
            }
            json_add_string(item, "name", name);
            json_add_string(item, "path", m->vroot);
            cJSON_AddBoolToObject(item, "is_dir", true);
            cJSON_AddNumberToObject(item, "size", 0);
            json_add_string(item, "mount", m->label[0] ? m->label : "ext");
            cJSON_AddItemToArray(entries, item);
            injected++;
        }
        ESP_LOGI(TAG, "list / : injected %d extra-mount entries", injected);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);
    return err;
}

static esp_err_t file_download_handler(httpd_req_t *req)
{
    const char *relative_path = req->uri + strlen("/files");
    if (!path_is_safe(relative_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[CONFIG_HTTP_PATH_MAX];
    if (resolve_storage_path(relative_path, full_path, sizeof(full_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st = {0};
    if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    FILE *file = fopen(full_path, "rb");
    if (!file) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    char *scratch = alloc_scratch_buffer();
    if (!scratch) {
        fclose(file);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    bool needs_spi_lock = match_extra_mount(relative_path) != NULL;
    while (!feof(file)) {
        bool spi_locked = false;
        if (needs_spi_lock) {
            spi_locked = (spi_bus_arbiter_lock(2000) == ESP_OK);
        }
        size_t read_bytes = fread(scratch, 1, CONFIG_HTTP_SCRATCH_SIZE, file);
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
    char relative_path[CONFIG_HTTP_PATH_MAX] = {0};
    if (query_get(req, "path", relative_path, sizeof(relative_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid upload size");
        return ESP_ERR_INVALID_SIZE;
    }
    /* Allow large uploads when the destination is an SD-card mount. */
    size_t max_size = match_extra_mount(relative_path)
                      ? CONFIG_HTTP_UPLOAD_MAX_SIZE_SD
                      : CONFIG_HTTP_UPLOAD_MAX_SIZE;
    if ((size_t)req->content_len > max_size) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Upload too large (max %u bytes)",
                 (unsigned)max_size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_ERR_INVALID_SIZE;
    }

    char full_path[CONFIG_HTTP_PATH_MAX];
    if (resolve_storage_path(relative_path, full_path, sizeof(full_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_ERR_INVALID_ARG;
    }

    char parent_path[CONFIG_HTTP_PATH_MAX];
    strlcpy(parent_path, full_path, sizeof(parent_path));
    char *slash = strrchr(parent_path, '/');
    if (!slash || slash == parent_path) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_ERR_INVALID_ARG;
    }
    *slash = '\0';

    struct stat st = {0};
    if (stat(parent_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Parent directory not found");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(full_path, "wb");
    if (!file) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_FAIL;
    }

    char *scratch = alloc_scratch_buffer();
    if (!scratch) {
        fclose(file);
        unlink(full_path);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    /* See spi_bus_arbiter rationale: lock around each fwrite() when target
     * is on a shared SPI bus (SD card) so SDSPI polling does not collide
     * with concurrent LCD DMA bursts. */
    bool needs_spi_lock = match_extra_mount(relative_path) != NULL;

    int remaining = req->content_len;
    while (remaining > 0) {
        int chunk = remaining > CONFIG_HTTP_SCRATCH_SIZE ? CONFIG_HTTP_SCRATCH_SIZE : remaining;
        int received = httpd_req_recv(req, scratch, chunk);
        if (received <= 0) {
            free(scratch);
            fclose(file);
            unlink(full_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
            return ESP_FAIL;
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
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        /* Yield so the IDLE task on this single-core target can run and
         * reset the watchdog. SDSPI writes at low frequency can keep the
         * httpd task hot for many seconds otherwise. */
        vTaskDelay(1);

        remaining -= received;
    }

    free(scratch);
    {
        bool spi_locked = false;
        if (needs_spi_lock) {
            spi_locked = (spi_bus_arbiter_lock(2000) == ESP_OK);
        }
        fclose(file);   /* may flush FAT cache to SDSPI */
        if (spi_locked) {
            spi_bus_arbiter_unlock();
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t files_delete_handler(httpd_req_t *req)
{
    char relative_path[CONFIG_HTTP_PATH_MAX] = {0};
    if (query_get(req, "path", relative_path, sizeof(relative_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[CONFIG_HTTP_PATH_MAX];
    if (resolve_storage_path(relative_path, full_path, sizeof(full_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st = {0};
    if (stat(full_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Path not found");
        return ESP_ERR_NOT_FOUND;
    }

    int rc = S_ISDIR(st.st_mode) ? rmdir(full_path) : unlink(full_path);
    if (rc != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t files_mkdir_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (parse_json_body(req, &root) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (!cJSON_IsString(path_item) || !path_is_safe(path_item->valuestring)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[CONFIG_HTTP_PATH_MAX];
    esp_err_t err = resolve_storage_path(path_item->valuestring, full_path, sizeof(full_path));
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return err;
    }

    if (mkdir(full_path, 0775) != 0 && errno != EEXIST) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create directory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t captive_404_handler(httpd_req_t *req, httpd_err_code_t error)
{
    if (!basic_demo_wifi_is_ap_active()) {
        return httpd_resp_send_err(req, error, NULL);
    }

    const char *ap_ip = basic_demo_wifi_get_ap_ip();
    if (!ap_ip || !ap_ip[0]) {
        ap_ip = "192.168.4.1";
    }

    char location[40];
    snprintf(location, sizeof(location), "http://%s/", ap_ip);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t config_http_server_init(const char *storage_base_path)
{
    if (!storage_base_path || storage_base_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_ctx.storage_base_path, storage_base_path, sizeof(s_ctx.storage_base_path));
    return ESP_OK;
}

esp_err_t config_http_server_register_mount(const char *vroot,
                                            const char *real_path,
                                            const char *label,
                                            bool (*alive_cb)(void))
{
    if (!vroot || vroot[0] == '\0') {
        return ESP_OK;  /* nothing to do */
    }
    if (vroot[0] != '/' || !real_path || real_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    int free_slot = -1;
    int match_slot = -1;
    for (int i = 0; i < CONFIG_HTTP_MAX_EXTRA_MOUNTS; ++i) {
        if (s_ctx.extra_mounts[i].vroot[0] == '\0') {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (strcmp(s_ctx.extra_mounts[i].vroot, vroot) == 0) {
            match_slot = i;
            break;
        }
    }
    int target = (match_slot >= 0) ? match_slot : free_slot;
    if (target < 0) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_ctx.extra_mounts[target], 0, sizeof(s_ctx.extra_mounts[target]));
    strlcpy(s_ctx.extra_mounts[target].vroot, vroot,
            sizeof(s_ctx.extra_mounts[target].vroot));
    strlcpy(s_ctx.extra_mounts[target].real_path, real_path,
            sizeof(s_ctx.extra_mounts[target].real_path));
    if (label) {
        strlcpy(s_ctx.extra_mounts[target].label, label,
                sizeof(s_ctx.extra_mounts[target].label));
    }
    s_ctx.extra_mounts[target].alive_cb = alive_cb;
    ESP_LOGI(TAG, "extra mount registered [slot=%d]: vroot=%s -> real=%s label=%s alive_cb=%p",
             target,
             s_ctx.extra_mounts[target].vroot,
             s_ctx.extra_mounts[target].real_path,
             s_ctx.extra_mounts[target].label[0] ? s_ctx.extra_mounts[target].label : "(none)",
             alive_cb);
    return ESP_OK;
}

esp_err_t config_http_server_unregister_mount(const char *vroot)
{
    if (!vroot || vroot[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < CONFIG_HTTP_MAX_EXTRA_MOUNTS; ++i) {
        if (s_ctx.extra_mounts[i].vroot[0] != '\0' &&
                strcmp(s_ctx.extra_mounts[i].vroot, vroot) == 0) {
            ESP_LOGI(TAG, "extra mount unregistered [slot=%d]: vroot=%s", i, vroot);
            memset(&s_ctx.extra_mounts[i], 0, sizeof(s_ctx.extra_mounts[i]));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t config_http_server_start(void)
{
    if (s_ctx.server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = BASIC_DEMO_HTTP_SERVER_PORT;
    config.ctrl_port = CONFIG_HTTP_CTRL_PORT;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_ctx.server, &config), TAG, "Failed to start HTTP server");

    httpd_uri_t handlers[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_handler },
        { .uri = "/index.html", .method = HTTP_GET, .handler = index_handler },
        { .uri = "/styles.css", .method = HTTP_GET, .handler = styles_handler },
        { .uri = "/app.js", .method = HTTP_GET, .handler = app_js_handler },
        { .uri = "/lean-qr.min.mjs", .method = HTTP_GET, .handler = lean_qr_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/wechat/login/start", .method = HTTP_POST, .handler = wechat_login_start_handler },
        { .uri = "/api/wechat/login/status", .method = HTTP_GET, .handler = wechat_login_status_handler },
        { .uri = "/api/wechat/login/cancel", .method = HTTP_POST, .handler = wechat_login_cancel_handler },
        { .uri = "/api/files", .method = HTTP_GET, .handler = files_list_handler },
        { .uri = "/api/files", .method = HTTP_DELETE, .handler = files_delete_handler },
        { .uri = "/api/files/upload", .method = HTTP_POST, .handler = files_upload_handler },
        { .uri = "/api/files/mkdir", .method = HTTP_POST, .handler = files_mkdir_handler },
        { .uri = "/files/*", .method = HTTP_GET, .handler = file_download_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_ctx.server, &handlers[i]),
                            TAG,
                            "Failed to register URI handler");
    }

    ESP_RETURN_ON_ERROR(httpd_register_err_handler(s_ctx.server,
                                                   HTTPD_404_NOT_FOUND,
                                                   captive_404_handler),
                        TAG, "Failed to register captive 404 handler");

    ESP_LOGI(TAG, "HTTP server started on port %d", BASIC_DEMO_HTTP_SERVER_PORT);
    return ESP_OK;
}

esp_err_t config_http_server_stop(void)
{
    if (!s_ctx.server) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(httpd_stop(s_ctx.server), TAG, "Failed to stop HTTP server");
    s_ctx.server = NULL;
    return ESP_OK;
}
