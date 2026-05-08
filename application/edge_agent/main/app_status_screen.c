/*
 * SPDX-FileCopyrightText: 2026 RockBase IoT (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_status_screen.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "spi_bus_arbiter.h"

#include "app_message_inbox.h"
#include "display_arbiter.h"
#include "esp_painter.h"
#include "esp_painter_font.h"
#include "wifi_manager.h"

static const char *TAG = "app_status_screen";

typedef enum {
    ASS_CMD_SHOW_PAGE = 0,
    ASS_CMD_REFRESH,
    ASS_CMD_HIDE,
} ass_cmd_kind_t;

typedef struct {
    ass_cmd_kind_t kind;
    app_status_page_t page;
    uint32_t hold_ms;
} ass_cmd_t;

typedef struct {
    bool inited;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    int width;
    int height;
    bool swap_rgb565;
    uint16_t *fb;             /* RGB565 framebuffer (canvas-sized) */
    size_t fb_bytes;
    /* Permanent DMA-capable bounce buffer for one stripe. Allocated once
     * at init() while the internal heap is still fresh, so SPI transfers
     * never have to fight a fragmented internal heap at runtime. */
    uint16_t *dma_stripe;
    size_t   dma_stripe_rows;
    esp_painter_handle_t painter;
    SemaphoreHandle_t lock;   /* serialize paint operations         */
    app_config_t settings;
    bool settings_valid;
    QueueHandle_t cmd_queue;
    TaskHandle_t  worker;
    app_status_page_t current_page;
    bool owner_held;
    int  msg_scroll;          /* scroll offset for messages page (>=0) */
} app_status_screen_state_t;

static app_status_screen_state_t s_state = {
    .current_page = APP_STATUS_PAGE_STATUS,
};

static void ass_page_worker(void *arg);

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
    s_state.io = handles->io_handle;
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
    /* The bundled basic_font_* tables only carry ASCII glyphs (0x20–0x7E).
     * Multi-byte UTF-8 sequences (CJK etc.) would otherwise be sliced byte
     * by byte and rendered as garbage glyphs. Until a real CJK font is
     * integrated, fold any non-ASCII byte to '?' so Chinese/emoji at least
     * render as a visible placeholder instead of pixel garbage. The first
     * non-ASCII byte of a UTF-8 multi-byte char becomes one '?', and the
     * trailing continuation bytes are dropped. */
    char ascii_buf[256];
    bool need_fold = false;
    for (const char *p = text; *p; ++p) {
        if ((unsigned char)*p >= 0x80) {
            need_fold = true;
            break;
        }
    }
    const char *to_draw = text;
    if (need_fold) {
        size_t o = 0;
        bool last_was_q = false;
        for (const char *p = text; *p && o + 1 < sizeof(ascii_buf); ++p) {
            unsigned char c = (unsigned char)*p;
            if (c < 0x80) {
                ascii_buf[o++] = (char)c;
                last_was_q = false;
            } else if ((c & 0xC0) != 0x80) {  /* lead byte of a UTF-8 char */
                if (!last_was_q) {
                    ascii_buf[o++] = '?';
                    last_was_q = true;
                }
            }
            /* continuation bytes (0x80..0xBF) are silently dropped */
        }
        ascii_buf[o] = '\0';
        to_draw = ascii_buf;
        if (to_draw[0] == '\0') {
            return;
        }
    }
    esp_err_t err = esp_painter_draw_string(s_state.painter,
                                            (uint8_t *)s_state.fb,
                                            (uint32_t)s_state.fb_bytes,
                                            (uint16_t)x, (uint16_t)y,
                                            font, color, to_draw);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
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
    /* Take the shared SPI-bus mutex around the entire flush so an SDSPI
     * polling transaction on the same host (e.g. an HTTP file upload to
     * /sdcard) cannot interleave with our queued LCD DMA bursts and trip
     * the spi_hal_setup_trans assert (`spi_ll_get_running_cmd(hw) == 0`). */
    bool spi_locked = (spi_bus_arbiter_lock(2000) == ESP_OK);
    /*
     * The framebuffer lives in PSRAM (not DMA-capable). If we hand a PSRAM
     * pointer to esp_lcd_panel_draw_bitmap, the SPI master tries to allocate
     * an internal-RAM bounce buffer the size of the transfer -- which fails
     * once the internal heap is fragmented (largest free block < 16 KiB
     * once Wi-Fi/TLS/JSON have warmed up).
     *
     * Solution: copy each stripe into the permanent dma_stripe buffer (which
     * was allocated in DMA-capable internal RAM at init time, while the heap
     * was still fresh) and submit that. SPI then has nothing to allocate.
     */
    int stripe_rows = (int)s_state.dma_stripe_rows;
    if (stripe_rows < 1) {
        stripe_rows = 8;
    }
    int y = 0;
    while (y < s_state.height) {
        int rows = stripe_rows;
        if (y + rows > s_state.height) {
            rows = s_state.height - y;
        }
        size_t row_pixels = (size_t)s_state.width;
        size_t chunk_pixels = row_pixels * (size_t)rows;
        const uint16_t *src = s_state.fb + (size_t)y * row_pixels;
        const uint16_t *submit;

        if (s_state.dma_stripe) {
            memcpy(s_state.dma_stripe, src, chunk_pixels * sizeof(uint16_t));
            submit = s_state.dma_stripe;
        } else {
            /* Fallback to PSRAM source if the bounce buffer wasn't
             * available at init -- SPI driver will try (and may fail) to
             * allocate one itself. */
            submit = src;
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(s_state.panel, 0, y,
                                                  s_state.width, y + rows,
                                                  submit);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "draw_bitmap stripe y=%d rows=%d failed: %s",
                     y, rows, esp_err_to_name(err));
            /* Stop on first failure to avoid cascading log spam. */
            if (spi_locked) {
                spi_bus_arbiter_unlock();
            }
            return;
        }
        /*
         * draw_bitmap queues the color transfer asynchronously and returns
         * immediately. If we let the loop continue and overwrite
         * dma_stripe before SPI has finished DMAing the previous stripe,
         * the in-flight transfer reads the NEXT stripe's pixels -- which
         * shows up as a vertical "echo"/doubling on the panel.
         *
         * esp_lcd_panel_io_tx_param() with lcd_cmd=-1 is documented to
         * drain any pending queued color transactions before returning,
         * without sending any actual command on the bus. That makes the
         * stripe loop effectively synchronous and guarantees dma_stripe is
         * safe to reuse on the next iteration.
         */
        if (s_state.io && submit == s_state.dma_stripe) {
            (void)esp_lcd_panel_io_tx_param(s_state.io, -1, NULL, 0);
        }
        y += rows;
    }
    if (spi_locked) {
        spi_bus_arbiter_unlock();
    }
}

/* Lazily allocate the panel-sized RGB565 framebuffer. Caller must hold
 * s_state.lock. Returns ESP_OK if the buffer is ready, or ESP_ERR_NO_MEM
 * if even the fallback DMA-capable allocation cannot be satisfied. */
static esp_err_t ass_ensure_fb_locked(void)
{
    if (s_state.fb) {
        return ESP_OK;
    }
    s_state.fb = heap_caps_aligned_alloc(64, s_state.fb_bytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_state.fb) {
        s_state.fb = heap_caps_aligned_alloc(64, s_state.fb_bytes,
                                             MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    if (!s_state.fb) {
        ESP_LOGW(TAG, "framebuffer alloc (%u B) failed - status overlay skipped",
                 (unsigned)s_state.fb_bytes);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Free the framebuffer to return ~150 KiB of PSRAM to the heap when the
 * status overlay is hidden. Caller must hold s_state.lock. */
static void ass_release_fb_locked(void)
{
    if (s_state.fb) {
        heap_caps_free(s_state.fb);
        s_state.fb = NULL;
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

static void ass_paint_unread_badge_in_header(void)
{
    size_t unread = app_message_inbox_unread_count();
    char badge[24];
    if (unread > 0) {
        snprintf(badge, sizeof(badge), "MSG %u", (unsigned)unread);
    } else {
        snprintf(badge, sizeof(badge), "MSG 0");
    }
    int w = ass_text_width(ASS_FONT_BODY, badge);
    int badge_w = w + 12;
    int badge_h = 22;
    int x = s_state.width - badge_w - 6;
    int y = 4;
    uint16_t bg = unread > 0 ? ass_rgb565(0xCC, 0x33, 0x33)
                             : ass_rgb565(0x35, 0x55, 0x90);
    ass_fill_rect(bg, x, y, badge_w, badge_h);
    ass_draw_text(ASS_FONT_BODY, ESP_PAINTER_COLOR_WHITE, x + 6, y + 3, badge);
}

static void ass_paint_unread_banner(int *y)
{
    size_t unread = app_message_inbox_unread_count();
    if (unread == 0) {
        return;
    }
    int banner_h = 28;
    /* Bright orange-red banner under the header: very hard to miss. */
    uint16_t bg = ass_rgb565(0xE0, 0x55, 0x1F);
    ass_fill_rect(bg, 0, *y, s_state.width, banner_h);
    char text[48];
    if (unread == 1) {
        snprintf(text, sizeof(text), "NEW message - Touch to view");
    } else {
        snprintf(text, sizeof(text), "%u NEW messages - Touch to view",
                 (unsigned)unread);
    }
    int w = ass_text_width(ASS_FONT_BODY, text);
    int x = (s_state.width - w) / 2;
    if (x < 4) {
        x = 4;
    }
    ass_draw_text(ASS_FONT_BODY, ESP_PAINTER_COLOR_WHITE, x, *y + 6, text);
    *y += banner_h + 4;
}

static void ass_paint_status(void)
{
    ass_clear(ASS_PANEL_BG_R, ASS_PANEL_BG_G, ASS_PANEL_BG_B);

    // Header bar
    ass_fill_rect(ass_rgb565(0x1E, 0x4C, 0xA8), 0, 0, s_state.width, 30);
    ass_draw_text(ASS_FONT_SUBTITLE, ESP_PAINTER_COLOR_WHITE, 8, 4, "ESP-Claw Status");
    ass_paint_unread_badge_in_header();

    int y = 38;
    ass_paint_unread_banner(&y);

    // ----- Wi-Fi block -----
    wifi_manager_status_t wifi = {0};
    wifi_manager_get_status(&wifi);
    bool sta_conn = wifi.sta_connected;
    const char *ip = wifi.sta_ip ? wifi.sta_ip : "-";

    // ass_paint_status_kv(&y, "WiFi:", wifi.mode ? wifi.mode : "off",
    //                     sta_conn ? ESP_PAINTER_COLOR_GREEN
    //                              : ESP_PAINTER_COLOR_YELLOW);

    if (s_state.settings_valid && ass_present(s_state.settings.wifi_ssid)) {
        ass_paint_status_kv(&y, "SSID:", s_state.settings.wifi_ssid,
                            ESP_PAINTER_COLOR_WHITE);
    } else {
        ass_paint_status_kv(&y, "SSID:", "(not set)", ESP_PAINTER_COLOR_DARKGREY);
    }

    ass_paint_status_kv(&y, "IP:", sta_conn ? ip : "-",
                        sta_conn ? ESP_PAINTER_COLOR_WHITE
                                 : ESP_PAINTER_COLOR_DARKGREY);

    if (wifi.ap_active) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s @ %s",
                 ass_or_unset(wifi.ap_ssid),
                 ass_or_unset(wifi.ap_ip));
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

    // ----- Memory block -----
    {
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t low_internal  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        char buf[64];
        snprintf(buf, sizeof(buf), "int %uK (lo %uK)",
                 (unsigned)(free_internal / 1024),
                 (unsigned)(low_internal / 1024));
        uint16_t color = (free_internal < 16 * 1024)  ? ESP_PAINTER_COLOR_RED
                         : (free_internal < 32 * 1024) ? ESP_PAINTER_COLOR_YELLOW
                         : ESP_PAINTER_COLOR_WHITE;
        ass_paint_status_kv(&y, "RAM:", buf, color);
        if (free_psram > 0 || heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
            snprintf(buf, sizeof(buf), "%uK free",
                     (unsigned)(free_psram / 1024));
            ass_paint_status_kv(&y, "PSRAM:", buf, ESP_PAINTER_COLOR_WHITE);
        }
    }

    // ----- Footer -----
    const esp_app_desc_t *desc = esp_app_get_description();
    char foot[80];
    snprintf(foot, sizeof(foot), "fw %s * uptime %lus",
             desc ? desc->version : "?",
             (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000));
    ass_draw_text(ASS_FONT_BODY, ESP_PAINTER_COLOR_DARKGREY,
                  8, s_state.height - 20, foot);
}

static const char *ass_pretty_channel(const char *channel)
{
    if (!channel) {
        return "?";
    }
    if (strcmp(channel, "feishu") == 0)   { return "Feishu"; }
    if (strcmp(channel, "telegram") == 0) { return "TG";     }
    if (strcmp(channel, "qq") == 0)       { return "QQ";     }
    if (strcmp(channel, "wechat") == 0)   { return "WeChat"; }
    return channel;
}

static esp_painter_color_t ass_channel_color(const char *channel)
{
    if (!channel) {
        return ESP_PAINTER_COLOR_LIGHTGREY;
    }
    if (strcmp(channel, "feishu") == 0)   { return ESP_PAINTER_COLOR_CYAN;        }
    if (strcmp(channel, "telegram") == 0) { return ESP_PAINTER_COLOR_BLUE;        }
    if (strcmp(channel, "qq") == 0)       { return ESP_PAINTER_COLOR_GREENYELLOW; }
    if (strcmp(channel, "wechat") == 0)   { return ESP_PAINTER_COLOR_GREEN;       }
    return ESP_PAINTER_COLOR_LIGHTGREY;
}

/* Truncate `src` so the rendered width fits in `max_px`. If truncated,
 * the result ends with "..." (1 byte per dot in this ASCII font). The font
 * used here is monospace (font->width per glyph), so width math is simple. */
static void ass_clip_text_to_width(const esp_painter_basic_font_t *font,
                                   const char *src, int max_px,
                                   char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!font || !src || max_px <= 0) {
        return;
    }
    int glyph_w = (int)font->width;
    if (glyph_w <= 0) {
        glyph_w = 1;
    }
    int max_chars = max_px / glyph_w;
    if (max_chars <= 0) {
        return;
    }
    size_t src_len = strlen(src);
    if ((int)src_len <= max_chars) {
        strlcpy(out, src, out_size);
        return;
    }
    if (max_chars <= 3 || out_size < 4) {
        size_t n = (size_t)max_chars;
        if (n >= out_size) {
            n = out_size - 1;
        }
        memcpy(out, src, n);
        out[n] = '\0';
        return;
    }
    /* Keep some head and a small tail so a long opaque ID like
     * "ou_6bac267caed7c1f8e016...e37" stays recognizable. */
    int keep = max_chars - 3;
    int tail = keep / 4;
    if (tail < 3) {
        tail = 3;
    }
    if (tail > 6) {
        tail = 6;
    }
    int head = keep - tail;
    if (head < 1) {
        head = 1;
    }
    if (head + tail + 3 > (int)out_size - 1) {
        size_t n = out_size - 1;
        memcpy(out, src, n);
        out[n] = '\0';
        return;
    }
    memcpy(out, src, head);
    out[head] = '.';
    out[head + 1] = '.';
    out[head + 2] = '.';
    memcpy(out + head + 3, src + src_len - tail, tail);
    out[head + 3 + tail] = '\0';
}

static void ass_paint_messages(void)
{
    ass_clear(ASS_PANEL_BG_R, ASS_PANEL_BG_G, ASS_PANEL_BG_B);

    // Header
    ass_fill_rect(ass_rgb565(0x1E, 0x4C, 0xA8), 0, 0, s_state.width, 30);
    char header[64];
    size_t total = app_message_inbox_count();
    snprintf(header, sizeof(header), "Messages  (%u)", (unsigned)total);
    ass_draw_text(ASS_FONT_SUBTITLE, ESP_PAINTER_COLOR_WHITE, 8, 4, header);

    int y = 36;
    const int row_h = 36;          /* 16 (line1) + 16 (line2) + 4 (gap) */
    const int footer_h = 22;
    int rows_avail = (s_state.height - 36 - footer_h) / row_h;
    if (rows_avail < 1) {
        rows_avail = 1;
    }

    if (total == 0) {
        ass_draw_text_centered(ASS_FONT_BODY, ESP_PAINTER_COLOR_DARKGREY,
                               s_state.height / 2 - 8, "(no messages yet)");
    } else {
        /* Clamp scroll offset to a valid range. */
        int max_scroll = (int)total - rows_avail;
        if (max_scroll < 0) {
            max_scroll = 0;
        }
        if (s_state.msg_scroll < 0) {
            s_state.msg_scroll = 0;
        }
        if (s_state.msg_scroll > max_scroll) {
            s_state.msg_scroll = max_scroll;
        }

        int first = s_state.msg_scroll;
        int last  = first + rows_avail - 1;
        if (last >= (int)total) {
            last = (int)total - 1;
        }

        for (int i = first; i <= last; i++) {
            app_message_inbox_entry_t e;
            if (!app_message_inbox_get((size_t)i, &e)) {
                break;
            }
            // separator line above each row
            ass_fill_rect(ass_rgb565(0x18, 0x24, 0x40),
                          0, y - 1, s_state.width, 1);

            /* ---- Line 1: channel + timestamp (right-aligned) ----- */
            const char *ch = ass_pretty_channel(e.channel);
            ass_draw_text(ASS_FONT_BODY, ass_channel_color(e.channel), 8, y, ch);

            /* Format timestamp as MM-DD HH:MM:SS using local time. */
            char ts_buf[24];
            ts_buf[0] = '\0';
            if (e.timestamp_ms > 0) {
                time_t secs = (time_t)(e.timestamp_ms / 1000);
                struct tm tm_info;
                if (localtime_r(&secs, &tm_info)) {
                    strftime(ts_buf, sizeof(ts_buf), "%m-%d %H:%M:%S", &tm_info);
                }
            }
            if (ts_buf[0] != '\0') {
                int ts_w = ass_text_width(ASS_FONT_BODY, ts_buf);
                int ts_x = s_state.width - 8 - ts_w;
                if (ts_x < 8 + ass_text_width(ASS_FONT_BODY, ch) + 8) {
                    ts_x = 8 + ass_text_width(ASS_FONT_BODY, ch) + 8;
                }
                ass_draw_text(ASS_FONT_BODY, ESP_PAINTER_COLOR_DARKGREY,
                              ts_x, y, ts_buf);
            }

            /* ---- Line 2: message text (single-line, truncated) ----- */
            int text_max_px = s_state.width - 16;
            char text_clip[64];
            ass_clip_text_to_width(ASS_FONT_BODY, e.text, text_max_px,
                                   text_clip, sizeof(text_clip));
            ass_draw_text(ASS_FONT_BODY, ESP_PAINTER_COLOR_WHITE,
                          8, y + 16, text_clip);

            y += row_h;
        }
    }

    // Footer: scroll indicator + hint
    char foot[80];
    if (total > 0) {
        int shown = (int)total - s_state.msg_scroll;
        if (shown > rows_avail) {
            shown = rows_avail;
        }
        int last_idx = s_state.msg_scroll + shown;
        snprintf(foot, sizeof(foot),
                 "%d-%d/%u  swipe up/down  unread:%u",
                 s_state.msg_scroll + 1, last_idx, (unsigned)total,
                 (unsigned)app_message_inbox_unread_count());
    } else {
        snprintf(foot, sizeof(foot),
                 "BOOT->next   unread:%u",
                 (unsigned)app_message_inbox_unread_count());
    }
    ass_draw_text(ASS_FONT_BODY, ESP_PAINTER_COLOR_DARKGREY,
                  8, s_state.height - 20, foot);
}

void app_status_screen_set_settings(const app_config_t *settings)
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
    /* Defer framebuffer allocation: it's ~150 KiB and only needed while the
     * status overlay is actually being drawn. We free it again as soon as the
     * overlay is hidden (see ass_release_fb_locked) so Lua scripts and other
     * components can reclaim that PSRAM. */
    s_state.fb = NULL;

    /* Allocate a permanent DMA-capable bounce buffer for one stripe so that
     * SPI transfers never depend on the runtime state of the (heavily
     * fragmented) internal heap. ~5 KiB at boot time is essentially free. */
    s_state.dma_stripe_rows = 8;
    {
        size_t bytes = (size_t)s_state.width * s_state.dma_stripe_rows * sizeof(uint16_t);
        s_state.dma_stripe = heap_caps_aligned_alloc(4, bytes,
                                                     MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_state.dma_stripe) {
            ESP_LOGW(TAG, "DMA stripe alloc (%u B) failed; falling back to PSRAM source",
                     (unsigned)bytes);
        }
    }

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

    s_state.cmd_queue = xQueueCreate(8, sizeof(ass_cmd_t));
    if (!s_state.cmd_queue) {
        vSemaphoreDelete(s_state.lock);
        s_state.lock = NULL;
        esp_painter_deinit(s_state.painter);
        s_state.painter = NULL;
        heap_caps_free(s_state.fb);
        s_state.fb = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_state.inited = true;
    ESP_LOGI(TAG, "status screen ready (fb=%u bytes)", (unsigned)s_state.fb_bytes);

    BaseType_t task_ok = xTaskCreate(ass_page_worker, "ass_page",
                                     ASS_TASK_STACK_SIZE, NULL,
                                     ASS_TASK_PRIORITY, &s_state.worker);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn page worker");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_status_screen_show_splash(uint32_t hold_ms)
{
    if (!s_state.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    esp_err_t err = ass_ensure_fb_locked();
    if (err == ESP_OK) {
        ass_paint_splash();
        ass_push_to_panel();
    }
    /* Splash is one-shot: free the framebuffer right away so the ~150 KiB
     * of PSRAM is available for the rest of boot (Lua scripts, JPEG decode,
     * skill loading, ...). */
    ass_release_fb_locked();
    xSemaphoreGive(s_state.lock);

    if (hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }
    return err;
}

typedef struct {
    uint32_t hold_ms;
} ass_status_task_arg_t;

static void ass_paint_current_locked(void)
{
    switch (s_state.current_page) {
    case APP_STATUS_PAGE_MESSAGES:
        ass_paint_messages();
        break;
    case APP_STATUS_PAGE_STATUS:
    default:
        ass_paint_status();
        break;
    }
    ass_push_to_panel();
}

static void ass_acquire_owner(void)
{
    if (s_state.owner_held) {
        return;
    }
    esp_err_t err = display_arbiter_acquire(DISPLAY_ARBITER_OWNER_LUA);
    if (err == ESP_OK) {
        s_state.owner_held = true;
    } else {
        ESP_LOGW(TAG, "acquire owner failed: %s", esp_err_to_name(err));
    }
}

static void ass_release_owner(void)
{
    if (!s_state.owner_held) {
        return;
    }
    /* Free the framebuffer first so the PSRAM is available the moment the
     * other display owner (emote / Lua scripts) takes over. */
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    ass_release_fb_locked();
    xSemaphoreGive(s_state.lock);

    display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
    s_state.owner_held = false;
    /* Mark all currently shown messages as read once we hand the screen back. */
    if (s_state.current_page == APP_STATUS_PAGE_MESSAGES) {
        app_message_inbox_mark_all_read();
    }
}

static void ass_page_worker(void *arg)
{
    (void)arg;
    TickType_t deadline = 0;     /* 0 == not currently visible              */
    bool visible = false;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait_ticks;
        if (visible) {
            if (deadline > now) {
                wait_ticks = deadline - now;
            } else {
                wait_ticks = 0;
            }
        } else {
            wait_ticks = portMAX_DELAY;
        }

        ass_cmd_t cmd;
        BaseType_t got = xQueueReceive(s_state.cmd_queue, &cmd, wait_ticks);

        if (got == pdFALSE) {
            /* Timed out -> hide. */
            ass_release_owner();
            visible = false;
            deadline = 0;
            continue;
        }

        switch (cmd.kind) {
        case ASS_CMD_SHOW_PAGE:
            if (cmd.page < APP_STATUS_PAGE_COUNT) {
                /* Reset scroll when switching pages. */
                if (s_state.current_page != cmd.page) {
                    s_state.msg_scroll = 0;
                }
                s_state.current_page = cmd.page;
            }
            ass_acquire_owner();
            xSemaphoreTake(s_state.lock, portMAX_DELAY);
            if (ass_ensure_fb_locked() == ESP_OK) {
                ass_paint_current_locked();
            }
            xSemaphoreGive(s_state.lock);
            visible = true;
            if (cmd.hold_ms == 0) {
                deadline = xTaskGetTickCount() + portMAX_DELAY / 2;
            } else {
                deadline = xTaskGetTickCount() + pdMS_TO_TICKS(cmd.hold_ms);
            }
            break;
        case ASS_CMD_REFRESH:
            if (visible) {
                xSemaphoreTake(s_state.lock, portMAX_DELAY);
                if (ass_ensure_fb_locked() == ESP_OK) {
                    ass_paint_current_locked();
                }
                xSemaphoreGive(s_state.lock);
            }
            break;
        case ASS_CMD_HIDE:
            ass_release_owner();
            visible = false;
            deadline = 0;
            break;
        }
    }
}

static esp_err_t ass_post_cmd(ass_cmd_kind_t kind, app_status_page_t page, uint32_t hold_ms)
{
    if (!s_state.inited || !s_state.cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    ass_cmd_t cmd = { .kind = kind, .page = page, .hold_ms = hold_ms };
    return xQueueSend(s_state.cmd_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_status_screen_show_page(app_status_page_t page, uint32_t hold_ms)
{
    if (page >= APP_STATUS_PAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return ass_post_cmd(ASS_CMD_SHOW_PAGE, page, hold_ms);
}

esp_err_t app_status_screen_next_page(void)
{
    app_status_page_t next = (app_status_page_t)((s_state.current_page + 1) % APP_STATUS_PAGE_COUNT);
    /* Default hold per page: status 5s, messages 8s. */
    uint32_t hold = (next == APP_STATUS_PAGE_MESSAGES) ? 8000U : 5000U;
    return app_status_screen_show_page(next, hold);
}

void app_status_screen_request_refresh(void)
{
    ass_post_cmd(ASS_CMD_REFRESH, s_state.current_page, 0);
}

void app_status_screen_scroll_messages(int delta)
{
    /* Only meaningful on the messages page; if not currently on it, just
     * adjust the offset so the next switch lands on the right spot. */
    s_state.msg_scroll += delta;
    if (s_state.msg_scroll < 0) {
        s_state.msg_scroll = 0;
    }
    if (s_state.current_page == APP_STATUS_PAGE_MESSAGES) {
        ass_post_cmd(ASS_CMD_REFRESH, s_state.current_page, 0);
    }
}

esp_err_t app_status_screen_request_status(uint32_t hold_ms)
{
    return app_status_screen_show_page(APP_STATUS_PAGE_STATUS, hold_ms);
}
