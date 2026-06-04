/*
 * SPDX-FileCopyrightText: 2026 RockBase-iot Technology (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "logo.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "display_arbiter.h"
#include "nanosvg_wrapper.h"
#include "spi_bus_arbiter.h"

static const char *TAG = "logo";

/**
 * Path on the FATFS partition where the custom SVG logo is stored.
 * The HTTP upload API writes to this location.
 */
#define LOGO_SVG_PATH "/fatfs/logo/custom.svg"

static esp_lcd_panel_handle_t s_panel_handle;
static int s_lcd_width;
static int s_lcd_height;
static bool s_logo_active;
static bool s_lcd_loaded;

/* Saved previous owner-changed callback (typically the emote's).
 * We chain to it when ownership transitions away from LOGO so the
 * emote performs a full refresh after logo deletion. */
static display_arbiter_owner_changed_cb_t s_prev_owner_cb;
static void *s_prev_owner_ctx;

/* File-change watch timer.  Checks the SVG mtime periodically and
 * re-rasterises only when it has changed (e.g. after a Lua skill writes
 * a new SVG). */
static esp_timer_handle_t s_watch_timer;
static time_t s_last_mtime;

/** Interval (microseconds) between file-change checks. */
#define LOGO_WATCH_INTERVAL_US (2 * 1000 * 1000)

static bool logo_should_swap_color(const dev_display_lcd_config_t *lcd_cfg)
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

static esp_err_t logo_load_lcd_handles(void)
{
#if !CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_lcd_loaded) {
        return ESP_OK;
    }

    void *lcd_handle = NULL;
    void *lcd_config = NULL;

    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle),
                        TAG, "failed to get LCD handle");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config),
                        TAG, "failed to get LCD config");

    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)lcd_config;

    if (!lcd_handles || !lcd_cfg || !lcd_handles->panel_handle) {
        ESP_LOGE(TAG, "LCD handles/config incomplete");
        return ESP_ERR_INVALID_STATE;
    }

    s_panel_handle = lcd_handles->panel_handle;
    s_lcd_width = lcd_cfg->lcd_width;
    s_lcd_height = lcd_cfg->lcd_height;
    s_lcd_loaded = true;

    ESP_LOGI(TAG, "LCD loaded: %dx%d", s_lcd_width, s_lcd_height);
    return ESP_OK;
#endif
}

static void logo_on_owner_changed(display_arbiter_owner_t owner, void *user_ctx)
{
    (void)user_ctx;

    if (owner == DISPLAY_ARBITER_OWNER_LOGO && s_logo_active) {
        /* Display ownership returned to LOGO -- re-push the bitmap.
         * This happens after a Lua script releases the display. */
        esp_err_t err = logo_refresh();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "re-display after owner change failed: %s", esp_err_to_name(err));
        }
        return;
    }

    /* Forward to the previous callback (e.g. emote) so it can redraw
     * when ownership transitions away from LOGO. */
    if (s_prev_owner_cb) {
        s_prev_owner_cb(owner, s_prev_owner_ctx);
    }
}

static esp_err_t logo_display_bitmap(uint16_t *buf)
{
    if (!s_panel_handle || !buf) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Serialise LCD DMA against shared-SPI SDSPI (same pattern as emote). */
    bool spi_locked = (spi_bus_arbiter_lock(2000) == ESP_OK);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle,
                  0, 0, s_lcd_width, s_lcd_height, buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_draw_bitmap failed: %s", esp_err_to_name(err));
    }
    if (spi_locked) {
        spi_bus_arbiter_unlock();
    }
    return err;
}

static void logo_watch_stop(void)
{
    if (s_watch_timer) {
        esp_timer_stop(s_watch_timer);  /* ignore error if not running */
    }
}

static time_t logo_get_mtime(void)
{
    struct stat st;
    if (stat(LOGO_SVG_PATH, &st) == 0 && S_ISREG(st.st_mode)) {
        return st.st_mtime;
    }
    return 0;
}

static void logo_watch_timer_cb(void *arg)
{
    (void)arg;

    if (!s_logo_active) {
        return;
    }

    time_t mtime = logo_get_mtime();
    if (mtime != 0 && mtime != s_last_mtime) {
        ESP_LOGI(TAG, "SVG file changed (mtime %ld -> %ld), refreshing",
                 (long)s_last_mtime, (long)mtime);
        s_last_mtime = mtime;
        esp_err_t err = logo_refresh();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "auto-refresh failed: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t logo_watch_start(void)
{
    if (s_watch_timer) {
        /* Restart the timer to reset the countdown. */
        esp_timer_stop(s_watch_timer);
        return esp_timer_start_periodic(s_watch_timer, LOGO_WATCH_INTERVAL_US);
    }

    const esp_timer_create_args_t args = {
        .callback = logo_watch_timer_cb,
        .name = "logo_watch",
    };
    esp_err_t err = esp_timer_create(&args, &s_watch_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to create watch timer: %s", esp_err_to_name(err));
        return err;
    }
    s_last_mtime = logo_get_mtime();
    return esp_timer_start_periodic(s_watch_timer, LOGO_WATCH_INTERVAL_US);
}

bool logo_file_exists(void)
{
    struct stat st;
    return (stat(LOGO_SVG_PATH, &st) == 0 && S_ISREG(st.st_mode));
}

const char *logo_get_path(void)
{
    return LOGO_SVG_PATH;
}

bool logo_is_active(void)
{
    return s_logo_active;
}

esp_err_t logo_refresh(void)
{
    ESP_RETURN_ON_ERROR(logo_load_lcd_handles(), TAG, "LCD not available");

    if (!logo_file_exists()) {
        ESP_LOGW(TAG, "no custom SVG at %s", LOGO_SVG_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    bool swap = true;
    void *lcd_config = NULL;
    if (esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config) == ESP_OK) {
        swap = logo_should_swap_color((const dev_display_lcd_config_t *)lcd_config);
    }

    uint16_t *buf = NULL;
    ESP_RETURN_ON_ERROR(nanosvg_rasterize_file_rgb565(LOGO_SVG_PATH, s_lcd_width, s_lcd_height, &buf, swap),
                        TAG, "SVG rasterization failed");

    /* Acquire display ownership (LOGO replaces emote idle animation). */
    esp_err_t err = display_arbiter_acquire(DISPLAY_ARBITER_OWNER_LOGO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "arbiter acquire failed: %s (drawing anyway)", esp_err_to_name(err));
    }

    err = logo_display_bitmap(buf);
    free(buf);

    if (err == ESP_OK) {
        s_logo_active = true;
        s_last_mtime = logo_get_mtime();
        logo_watch_start();
        ESP_LOGI(TAG, "logo displayed (%dx%d)", s_lcd_width, s_lcd_height);
    }
    return err;
}

esp_err_t logo_stop(void)
{
    if (!s_logo_active) {
        return ESP_OK;
    }

    logo_watch_stop();
    s_logo_active = false;

    /* Restore the previous callback BEFORE releasing ownership, so the
     * emote's callback fires correctly on the ownership transition. */
    display_arbiter_set_owner_changed_callback(s_prev_owner_cb, s_prev_owner_ctx);
    s_prev_owner_cb = NULL;
    s_prev_owner_ctx = NULL;

    esp_err_t err = display_arbiter_release(DISPLAY_ARBITER_OWNER_LOGO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "arbiter release failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "logo stopped, emote restored");
    return err;
}

esp_err_t logo_start(void)
{
    esp_err_t err = logo_load_lcd_handles();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no LCD, logo disabled");
        return ESP_OK; /* not fatal */
    }

    /* Save the previous callback (typically the emote's) and install ours.
     * We chain to it in logo_on_owner_changed() when ownership leaves LOGO
     * so the emote performs a full refresh after logo deletion. */
    s_prev_owner_cb = NULL;
    s_prev_owner_ctx = NULL;
    display_arbiter_get_owner_changed_callback(&s_prev_owner_cb, &s_prev_owner_ctx);
    display_arbiter_set_owner_changed_callback(logo_on_owner_changed, NULL);

    if (logo_file_exists()) {
        err = logo_refresh();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "initial logo display failed: %s", esp_err_to_name(err));
            /* Fall back to emote */
        }
    } else {
        ESP_LOGI(TAG, "no custom logo, using emote idle");
    }

    return ESP_OK;
}
