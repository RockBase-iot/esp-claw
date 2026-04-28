/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t styles_css_start[] asm("_binary_styles_css_start");
extern const uint8_t styles_css_end[] asm("_binary_styles_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t lean_qr_min_mjs_start[] asm("_binary_lean_qr_min_mjs_start");
extern const uint8_t lean_qr_min_mjs_end[] asm("_binary_lean_qr_min_mjs_end");

static esp_err_t index_handler(httpd_req_t *req)
{
    return http_server_send_embedded_file(req, index_html_start, index_html_end, "text/html; charset=utf-8");
}

static esp_err_t styles_handler(httpd_req_t *req)
{
    return http_server_send_embedded_file(req, styles_css_start, styles_css_end, "text/css; charset=utf-8");
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    return http_server_send_embedded_file(req, app_js_start, app_js_end, "application/javascript; charset=utf-8");
}

static esp_err_t lean_qr_handler(httpd_req_t *req)
{
    return http_server_send_embedded_file(req, lean_qr_min_mjs_start, lean_qr_min_mjs_end, "application/javascript; charset=utf-8");
}

esp_err_t http_server_register_assets_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_handler },
        { .uri = "/index.html", .method = HTTP_GET, .handler = index_handler },
        { .uri = "/styles.css", .method = HTTP_GET, .handler = styles_handler },
        { .uri = "/app.js", .method = HTTP_GET, .handler = app_js_handler },
        { .uri = "/lean-qr.min.mjs", .method = HTTP_GET, .handler = lean_qr_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
