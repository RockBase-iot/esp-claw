/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_status_screen.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "basic_demo_wifi.h"
#include "display_arbiter.h"
#include "esp_painter.h"
#include "esp_painter_font.h"

static const char *TAG = "app_status_screen";

typedef struct {
    bool inited;
    esp_lcd_panel_handle_t panel;
    int width;
    int height;
    bool swap_rgb565;
    uint16_t *fb;             // RGB565 framebuffer (canvas-sized)
    size_t fb_bytes;
    esp_painter_handle_t painter;
    SemaphoreHandle_t lock;   // serialize concurrent paint requests
    basic_demo_settings_t settings;
    bool settings_valid;
} app_status_screen_state_t;

static app_status_screen_state_t s_state;

#define ASS_BG_R              0x10
#define ASS_BG_G              0x12
#define ASS_BG_B              0x20

#define ASS_PANEL_BG_R        0x0A
#define ASS_PANEL_BG_G        0x14
#define ASS_PANEL_BG_B        0x28

#define ASS_FONT_TITLE        (&esp_painter_basic_font_24)
#define ASS_FONT_SUBTITLE     (&esp_painter_basic_font_20)
#define ASS_FONT_BODY         (&esp_painter_basic_font_16)

#define ASS_TASK_STACK_SIZE   (4 * 1024)
#define ASS_TASK_PRIORITY     3

static bool ass_should_swap_color(const dev_display_lcd_config_t *lcd_cfg)
{
    if (lcd_cfg == NULL || lcd_cfg->sub_type == NULL) {
        return true;
    }

    if (strcmp(lcd_cfg->sub_type, "dsi") == 0 ||
            strcmp(lcd_cfg->sub_type, "mipi_dsi") == 0 ||
            strcmp(lcd_cfg->sub_type, "rgb") == 0) {
        return false;
    }

    return true;
}

static esp_err_t ass_load_panel(void)
{
#if !CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    ESP_LOGW(TAG, "Board has no display_lcd device, status screen disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    void *lcd_handle = NULL;
    void *lcd_config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                                                        &lcd_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config);
    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_FALSE(lcd_handle && lcd_config, ESP_ERR_INVALID_STATE, TAG,
                        "display_lcd handle/config is NULL");

    dev_display_lcd_handles_t *handles = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t *cfg = (dev_display_lcd_config_t *)lcd_config;
    ESP_RETURN_ON_FALSE(handles->panel_handle, ESP_ERR_INVALID_STATE, TAG,
                        "panel_handle is NULL");

    s_state.panel = handles->panel_handle;
    s_state.width = cfg->lcd_width;
    s_state.height = cfg->lcd_height;
    s_state.swap_rgb565 = ass_should_swap_color(cfg);
    ESP_LOGI(TAG, "panel=%p size=%dx%d swap=%d",
             s_state.panel, s_state.width, s_state.height, s_state.swap_rgb565);
    return ESP_OK;
#endif
}

static uint16_t ass_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)((((uint16_t)r & 0xF8) << 8) |
                            (((uint16_t)g & 0xFC) << 3) |
                            ((uint16_t)b >> 3));
    if (s_state.swap_rgb565) {
        v = (uint16_t)((v << 8) | (v >> 8));
    }
    return v;
}

static void ass_fill_rect(uint16_t color, int x, int y, int w, int h)
{
    if (!s_state.fb) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > s_state.width) {
        w = s_state.width - x;
    }
    if (y + h > s_state.height) {
        h = s_state.height - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int row = 0; row < h; row++) {
        uint16_t *line = s_state.fb + (size_t)(y + row) * s_state.width + x;
        for (int col = 0; col < w; col++) {
            line[col] = color;
        }
    }
}

static void ass_clear(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color = ass_rgb565(r, g, b);
    ass_fill_rect(color, 0, 0, s_state.width, s_state.height);
}

static void ass_draw_text(const esp_painter_basic_font_t *font,
                          esp_painter_color_t color, int x, int y, const char *text)
{
    if (!s_state.painter || !text || text[0] == '\0') {
        return;
    }
    if (y < 0 || y >= s_state.height) {
        return;
    }
    esp_err_t err = esp_painter_draw_string(s_state.painter,
                                            (uint8_t *)s_state.fb,
                                            (uint32_t)s_state.fb_bytes,
                                            (uint16_t)x, (uint16_t)y,
                                            font, color, text);
    if (err != ESP_OK && err != ESP_OK) {
        ESP_LOGD(TAG, "draw_string failed at (%d,%d): %s", x, y, esp_err_to_name(err));
    }
}

static int ass_text_width(const esp_painter_basic_font_t *font, const char *text)
{
    // Basic estimate: monospace font glyph width = font->width.
    if (!font || !text) {
        return 0;
    }
    return (int)font->width * (int)strlen(text);
}

static void ass_draw_text_centered(const esp_painter_basic_font_t *font,
                                   esp_painter_color_t color, int y, const char *text)
{
    int tw = ass_text_width(font, text);
    int x = (s_state.width - tw) / 2;
    if (x < 0) {
        x = 0;
    }
    ass_draw_text(font, color, x, y, text);
}

static void ass_push_to_panel(void)
{
    if (!s_state.panel || !s_state.fb) {
        return;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_state.panel, 0, 0,
                                              s_state.width, s_state.height,
                                              s_state.fb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
    }
}

static void ass_paint_splash(void)
{
    ass_clear(ASS_BG_R, ASS_BG_G, ASS_BG_B);

    // Top accent bar.
    ass_fill_rect(ass_rgb565(0x2C, 0x80, 0xFF), 0, 0, s_state.width, 4);
    ass_fill_rect(ass_rgb565(0x2C, 0x80, 0xFF), 0, s_state.height - 4, s_state.width, 4);

    int title_y = s_state.height / 2 - 40;
    ass_draw_text_centered(ASS_FONT_TITLE, ESP_PAINTER_COLOR_WHITE, title_y, "ESP-Claw");

    ass_draw_text_centered(ASS_FONT_SUBTITLE, ESP_PAINTER_COLOR_CYAN,
                           title_y + 36, "Booting...");

    const esp_app_desc_t *desc = esp_app_get_description();
    char buf[64];
    snprintf(buf, sizeof(buf), "fw %s", desc ? desc->version : "?");
    ass_draw_text_centered(ASS_FONT_BODY, ESP_PAINTER_COLOR_LIGHTGREY,
                           title_y + 80, buf);

    ass_draw_text_centered(ASS_FONT_BODY, ESP_PAINTER_COLOR_DARKGREY,
                           s_state.height - 22, "NM-CYD-C5  *  ESP32-C5");
}

static const char *ass_or_unset(const char *s)
{
    return (s && s[0] != '\0') ? s : "(unset)";
}

static bool ass_present(const char *s)
{
    return s && s[0] != '\0';
}

static void ass_paint_status_kv(int *y, const char *label, const char *value,
                                esp_painter_color_t value_color)
{
    int row_y = *y;
    ass_draw_text(ASS_FONT_BODY, ESP_PAINTER_COLOR_CYAN, 8, row_y, label);
    int label_w = ass_text_width(ASS_FONT_BODY, label);
    ass_draw_text(ASS_FONT_BODY, value_color, 8 + label_w + 4, row_y,
                  value ? value : "");
    *y = row_y + 18;
}

static void ass_paint_im_row(int *y)
{
    int row_y = *y;
    ass_draw_text(ASS_FONT_BODY, ESP_PAINTER_COLOR_CYAN, 8, row_y, "IM:");

    bool feishu = ass_present(s_state.settings.feishu_app_id) &&
                  ass_present(s_state.settings.feishu_app_secret);
    bool tg     = ass_present(s_state.settings.tg_bot_token);
    bool qq     = ass_present(s_state.settings.qq_app_id) &&
                  ass_present(s_state.settings.qq_app_secret);
    bool wechat = ass_present(s_state.settings.wechat_token) &&
                  ass_present(s_state.settings.wechat_base_url);

    int x = 8 + ass_text_width(ASS_FONT_BODY, "IM:") + 8;
    struct {
        const char *name;
        bool ok;
    } items[] = {
        { "Feishu", feishu },
        { "TG", tg },
        { "QQ", qq },
        { "WeChat", wechat },
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        ass_draw_text(ASS_FONT_BODY, ESP_PAINTER_COLOR_WHITE, x, row_y, items[i].name);
        int name_w = ass_text_width(ASS_FONT_BODY, items[i].name);
        ass_draw_text(ASS_FONT_BODY,
                      items[i].ok ? ESP_PAINTER_COLOR_GREEN : ESP_PAINTER_COLOR_DARKGREY,
                      x + name_w + 2, row_y,
                      items[i].ok ? "+" : "-");
        x += name_w + 18;
    }

    *y = row_y + 18;
}

static void ass_paint_status(void)
{
    ass_clear(ASS_PANEL_BG_R, ASS_PANEL_BG_G, ASS_PANEL_BG_B);

    // Header bar
    ass_fill_rect(ass_rgb565(0x1E, 0x4C, 0xA8), 0, 0, s_state.width, 30);
    ass_draw_text(ASS_FONT_SUBTITLE, ESP_PAINTER_COLOR_WHITE, 8, 4, "ESP-Claw Status");

    int y = 38;

    // ----- Wi-Fi block -----
    const char *mode = basic_demo_wifi_get_mode_string();
    bool sta_conn = basic_demo_wifi_is_connected();
    const char *ip = basic_demo_wifi_get_ip();

    ass_paint_status_kv(&y, "WiFi:", mode ? mode : "off",
                        sta_conn ? ESP_PAINTER_COLOR_GREEN
                                 : ESP_PAINTER_COLOR_YELLOW);

    if (s_state.settings_valid && ass_present(s_state.settings.wifi_ssid)) {
        ass_paint_status_kv(&y, "SSID:", s_state.settings.wifi_ssid,
                            ESP_PAINTER_COLOR_WHITE);
    } else {
        ass_paint_status_kv(&y, "SSID:", "(not set)", ESP_PAINTER_COLOR_DARKGREY);
    }

    ass_paint_status_kv(&y, "IP:", sta_conn ? ip : "-",
                        sta_conn ? ESP_PAINTER_COLOR_WHITE
                                 : ESP_PAINTER_COLOR_DARKGREY);

    if (basic_demo_wifi_is_ap_active()) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s @ %s",
                 ass_or_unset(basic_demo_wifi_get_ap_ssid()),
                 ass_or_unset(basic_demo_wifi_get_ap_ip()));
        ass_paint_status_kv(&y, "AP:", buf, ESP_PAINTER_COLOR_ORANGE);
    }

    y += 4;

    // ----- LLM block -----
    if (s_state.settings_valid) {
        const char *profile = ass_present(s_state.settings.llm_profile)
                              ? s_state.settings.llm_profile : "(unset)";
        bool llm_ok = ass_present(s_state.settings.llm_api_key) &&
                      ass_present(s_state.settings.llm_model);
        ass_paint_status_kv(&y, "LLM:", profile,
                            llm_ok ? ESP_PAINTER_COLOR_GREEN
                                   : ESP_PAINTER_COLOR_YELLOW);
        ass_paint_status_kv(&y, "Model:", ass_or_unset(s_state.settings.llm_model),
                            ESP_PAINTER_COLOR_WHITE);
    } else {
        ass_paint_status_kv(&y, "LLM:", "(no settings)", ESP_PAINTER_COLOR_DARKGREY);
    }

    y += 4;

    // ----- IM block -----
    if (s_state.settings_valid) {
        ass_paint_im_row(&y);
    }

    // ----- Footer -----
    const esp_app_desc_t *desc = esp_app_get_description();
    char foot[80];
    snprintf(foot, sizeof(foot), "fw %s  *  uptime %lus",
             desc ? desc->version : "?",
             (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000));
    ass_draw_text(ASS_FONT_BODY, ESP_PAINTER_COLOR_DARKGREY,
                  8, s_state.height - 20, foot);
}

void app_status_screen_set_settings(const basic_demo_settings_t *settings)
{
    if (!settings) {
        return;
    }
    if (s_state.lock) {
        xSemaphoreTake(s_state.lock, portMAX_DELAY);
    }
    memcpy(&s_state.settings, settings, sizeof(s_state.settings));
    s_state.settings_valid = true;
    if (s_state.lock) {
        xSemaphoreGive(s_state.lock);
    }
}

esp_err_t app_status_screen_init(void)
{
    if (s_state.inited) {
        return ESP_OK;
    }

    esp_err_t err = ass_load_panel();
    if (err != ESP_OK) {
        return err;
    }

    s_state.fb_bytes = (size_t)s_state.width * s_state.height * sizeof(uint16_t);
    s_state.fb = heap_caps_aligned_alloc(64, s_state.fb_bytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_state.fb) {
        s_state.fb = heap_caps_aligned_alloc(64, s_state.fb_bytes,
                                             MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_state.fb, ESP_ERR_NO_MEM, TAG,
                        "failed to allocate %u-byte framebuffer",
                        (unsigned)s_state.fb_bytes);

    esp_painter_config_t pcfg = {
        .canvas = { .width = (uint16_t)s_state.width, .height = (uint16_t)s_state.height },
        .color_format = ESP_PAINTER_COLOR_FORMAT_RGB565,
        .default_font = ASS_FONT_BODY,
        .swap_rgb565 = s_state.swap_rgb565,
    };
    err = esp_painter_init(&pcfg, &s_state.painter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_painter_init failed: %s", esp_err_to_name(err));
        heap_caps_free(s_state.fb);
        s_state.fb = NULL;
        return err;
    }

    s_state.lock = xSemaphoreCreateMutex();
    if (!s_state.lock) {
        esp_painter_deinit(s_state.painter);
        s_state.painter = NULL;
        heap_caps_free(s_state.fb);
        s_state.fb = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_state.inited = true;
    ESP_LOGI(TAG, "status screen ready (fb=%u bytes)", (unsigned)s_state.fb_bytes);
    return ESP_OK;
}

esp_err_t app_status_screen_show_splash(uint32_t hold_ms)
{
    if (!s_state.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    ass_paint_splash();
    ass_push_to_panel();
    xSemaphoreGive(s_state.lock);

    if (hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }
    return ESP_OK;
}

typedef struct {
    uint32_t hold_ms;
} ass_status_task_arg_t;

static void ass_status_task(void *arg)
{
    ass_status_task_arg_t *targ = (ass_status_task_arg_t *)arg;
    uint32_t hold_ms = targ ? targ->hold_ms : 4000;
    free(targ);

    esp_err_t err = display_arbiter_acquire(DISPLAY_ARBITER_OWNER_LUA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "acquire owner failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    ass_paint_status();
    ass_push_to_panel();
    xSemaphoreGive(s_state.lock);

    if (hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }

    display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
    vTaskDelete(NULL);
}

esp_err_t app_status_screen_request_status(uint32_t hold_ms)
{
    if (!s_state.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    ass_status_task_arg_t *arg = malloc(sizeof(*arg));
    if (!arg) {
        return ESP_ERR_NO_MEM;
    }
    arg->hold_ms = hold_ms;
    BaseType_t ok = xTaskCreate(ass_status_task, "ass_status",
                                ASS_TASK_STACK_SIZE, arg,
                                ASS_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        free(arg);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
