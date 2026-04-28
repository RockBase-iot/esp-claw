/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#if defined(CONFIG_BASIC_DEMO_ENABLE_EMOTE)
#include "app_expression_emote.h"
#endif
#include "app_boot_button.h"
#include "app_message_inbox.h"
#include "app_status_screen.h"
#include "app_touch_xpt2046.h"
#include "basic_demo_settings.h"
#include "basic_demo_wifi.h"
#include "cap_lua.h"
#include "captive_dns.h"
#include "claw_skill.h"
#include "config_http_server.h"
#include "esp_err.h"
#include "esp_log.h"
#include <stdint.h>
#include "time.h"
#include "esp_vfs_fat.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wear_levelling.h"
#include "esp_board_manager_includes.h"
#include "claw_event_publisher.h"

static const char *TAG = "basic_demo";
static basic_demo_settings_t s_settings = {0};

#define BASIC_DEMO_BOOT_BUTTON_GPIO   28

/* NM-CYD-C5 on-board XPT2046 resistive touch (shares SPI2 with the LCD).
 * Calibration values are taken from the TFT_eSPI reference for this panel
 * in landscape (rotation=1): { xMin, xMax, yMin, yMax, swap_xy }.
 * Tweak in the field if the reported coordinates are offset. */
#define BASIC_DEMO_TOUCH_CS_GPIO      1
#define BASIC_DEMO_TOUCH_SCREEN_W     320
#define BASIC_DEMO_TOUCH_SCREEN_H     240

static void on_touch_tap(int x, int y, int pressure, void *user_ctx)
{
    (void)user_ctx;
    /* The user explicitly asked: only print, do NOT change the page. */
    ESP_LOGI(TAG, "touch tap (%d, %d) pressure=%d", x, y, pressure);
}

static void on_touch_gesture(app_touch_gesture_t gesture,
                             int sx, int sy, int ex, int ey, void *user_ctx)
{
    (void)user_ctx;
    (void)sx; (void)sy; (void)ex; (void)ey;
    switch (gesture) {
    case APP_TOUCH_GESTURE_SWIPE_UP:
        /* Swipe up -> show older messages (scroll list down by one row). */
        ESP_LOGI(TAG, "swipe up -> messages scroll +1");
        app_status_screen_show_page(APP_STATUS_PAGE_MESSAGES, 8000);
        app_status_screen_scroll_messages(+1);
        break;
    case APP_TOUCH_GESTURE_SWIPE_DOWN:
        /* Swipe down -> show newer messages. */
        ESP_LOGI(TAG, "swipe down -> messages scroll -1");
        app_status_screen_show_page(APP_STATUS_PAGE_MESSAGES, 8000);
        app_status_screen_scroll_messages(-1);
        break;
    case APP_TOUCH_GESTURE_SWIPE_LEFT:
    case APP_TOUCH_GESTURE_SWIPE_RIGHT:
    case APP_TOUCH_GESTURE_TAP:
    default:
        break;
    }
}

static void on_message_inbox_changed(void *user_ctx)
{
    (void)user_ctx;
    /* Inbox grew -> redraw current page (e.g. unread badge bumps), and pop
     * the status page for a few seconds so the new-message banner is visible
     * even when the emote view currently owns the screen. */
    app_status_screen_request_refresh();
    app_status_screen_show_page(APP_STATUS_PAGE_STATUS, 5000);
}

static void on_im_message_observed(const char *channel,
                                   const char *chat_id,
                                   const char *sender_id,
                                   const char *text,
                                   int64_t timestamp_ms,
                                   void *user_ctx)
{
    (void)chat_id;
    (void)user_ctx;
    app_message_inbox_record(channel, sender_id, text, timestamp_ms);
}

static void on_boot_button_pressed(void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "BOOT pressed -> next page");
    app_status_screen_next_page();
}

const char *basic_demo_fatfs_base_path = "/fatfs";
#define BASIC_DEMO_FATFS_PARTITION_LABEL "storage"
#define BASIC_DEMO_ENABLE_MEM_LOG        (0)

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static esp_err_t cap_lua_run_deactivate_guard(const char *session_id,
                                              const char *skill_id,
                                              char *reason_out,
                                              size_t reason_size)
{
    (void)session_id;
    (void)skill_id;

    size_t active = cap_lua_get_active_async_job_count();
    if (active == 0) {
        return ESP_OK;
    }
    if (reason_out && reason_size > 0) {
        if (active == SIZE_MAX) {
            snprintf(reason_out, reason_size,
                     "Lua async runner is busy (lock contended). "
                     "Call lua_list_async_jobs to confirm, then "
                     "lua_stop_all_async_jobs before retrying deactivate_skill.");
        } else {
            snprintf(reason_out, reason_size,
                     "%u Lua async job(s) still running. "
                     "Call lua_stop_all_async_jobs (or lua_stop_async_job per id/name) "
                     "first, then retry deactivate_skill.",
                     (unsigned)active);
        }
    }
    return ESP_ERR_INVALID_STATE;
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    const char *ap_ssid = basic_demo_wifi_is_ap_active()
                          ? basic_demo_wifi_get_ap_ssid()
                          : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             connected,
             basic_demo_wifi_is_ap_active(),
             basic_demo_wifi_get_mode_string(),
             ap_ssid ? ap_ssid : "(none)");

#if defined(CONFIG_BASIC_DEMO_ENABLE_EMOTE)
    esp_err_t err = app_expression_emote_set_status(connected, ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network emote: %s", esp_err_to_name(err));
    }
#else
    (void)connected;
#endif

    /* Keep settings cached for when the user opens the status page via BOOT.
     * Do NOT auto-pop the status overlay on every Wi-Fi event: it steals the
     * display owner from the emote at the very moment the emote is trying to
     * transition to "swim/Wi-Fi connected", and the gfx engine then keeps
     * flushing its cached "offline" frame after the overlay expires.
     * Press BOOT to view the structured status page on demand. */
    app_status_screen_set_settings(&s_settings);
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_fatfs(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t err;

    err = esp_vfs_fat_spiflash_mount_rw_wl(basic_demo_fatfs_base_path,
                                           BASIC_DEMO_FATFS_PARTITION_LABEL,
                                           &mount_config,
                                           &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_vfs_fat_info(basic_demo_fatfs_base_path, &total, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to query FATFS info: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "FATFS mounted total=%u used=%u",
                 (unsigned int)total,
                 (unsigned int)(total - free_bytes));
    }

    return ESP_OK;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t err = ESP_OK;

    if (!timezone || timezone[0] == '\0') {
        ESP_LOGE(TAG, "Timezone is empty.");
        err = ESP_ERR_INVALID_ARG;
        goto tz_default;
    }

    if (setenv("TZ", timezone, 1) != 0) {
        ESP_LOGE(TAG, "Failed to set TZ env");
        err = ESP_FAIL;
        goto tz_default;
    }
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return err;
}

#if BASIC_DEMO_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting basic_demo");
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(basic_demo_settings_init());
    ESP_ERROR_CHECK(basic_demo_settings_load(&s_settings));
    init_timezone(s_settings.time_timezone); // no need to check error
    ESP_ERROR_CHECK(esp_board_manager_init());

    /* Message inbox + IM event observer must be ready before any IM cap starts. */
    ESP_ERROR_CHECK(app_message_inbox_init());
    app_message_inbox_set_change_callback(on_message_inbox_changed, NULL);
    claw_event_router_set_message_observer(on_im_message_observed, NULL);

    /* Boot splash: rendered to the LCD before emote takes over the panel. */
    esp_err_t splash_err = app_status_screen_init();
    if (splash_err == ESP_OK) {
        app_status_screen_set_settings(&s_settings);
        app_status_screen_show_splash(2000);
    } else if (splash_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "status screen init failed: %s", esp_err_to_name(splash_err));
    }

    /* BOOT key (GPIO28) cycles through the on-screen pages. */
    esp_err_t btn_err = app_boot_button_init(BASIC_DEMO_BOOT_BUTTON_GPIO,
                                             on_boot_button_pressed, NULL);
    if (btn_err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT button init failed: %s", esp_err_to_name(btn_err));
    }

    /* XPT2046 resistive touch -> just log the screen-mapped coordinates. */
    {
        app_touch_xpt2046_config_t tcfg = {
            .host          = SPI2_HOST,
            .cs_gpio       = BASIC_DEMO_TOUCH_CS_GPIO,
            .irq_gpio      = -1,
            .screen_width  = BASIC_DEMO_TOUCH_SCREEN_W,
            .screen_height = BASIC_DEMO_TOUCH_SCREEN_H,
            .calibration = {
                /* TFT_eSPI rotation=1 calData: {200, 3428, 345, 3437, 1}.
                 * Empirical check on this panel: top-left raw (~398, ~454)
                 * mapped to (319, 239) without inversion, so both axes need
                 * to be flipped to align with screen origin (top-left). */
                .raw_x_min = 200,
                .raw_x_max = 3428,
                .raw_y_min = 345,
                .raw_y_max = 3437,
                .swap_xy   = true,
                .invert_x  = true,
                .invert_y  = true,
            },
        };
        esp_err_t touch_err = app_touch_xpt2046_init(&tcfg, on_touch_tap, NULL);
        if (touch_err != ESP_OK) {
            ESP_LOGW(TAG, "touch init failed: %s", esp_err_to_name(touch_err));
        } else {
            app_touch_xpt2046_set_gesture_callback(on_touch_gesture, NULL);
        }
    }

#if defined(CONFIG_BASIC_DEMO_ENABLE_EMOTE)
    ESP_ERROR_CHECK(app_expression_emote_start());
#endif
    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(basic_demo_wifi_init());
    ESP_ERROR_CHECK(config_http_server_init(basic_demo_fatfs_base_path));
    ESP_ERROR_CHECK(basic_demo_wifi_register_state_callback(on_wifi_state_changed, NULL));

    esp_err_t wifi_err = basic_demo_wifi_start(s_settings.wifi_ssid, s_settings.wifi_password);
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(config_http_server_start());

        if (s_settings.wifi_ssid[0] != '\0') {
            if (basic_demo_wifi_wait_connected(30000) == ESP_OK) {
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", basic_demo_wifi_get_ip());
            } else {
                ESP_LOGW(TAG, "STA could not connect, dropped to AP fallback");
            }
        }

        /* Captive DNS is only useful when the soft-AP is running (pure-AP or
         * AP-fallback after STA exhausts retries). In STA-only mode the AP
         * never starts, so skip it to avoid binding :53 unnecessarily. */
        if (basic_demo_wifi_is_ap_active()) {
            if (captive_dns_start() != ESP_OK) {
                ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
            }
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (open) IP=%s URL=http://%s/ ***",
                     basic_demo_wifi_get_ap_ssid(),
                     basic_demo_wifi_get_ap_ip(),
                     basic_demo_wifi_get_ap_ip());
        }
    }

    ESP_ERROR_CHECK(app_claw_start(&s_settings));

    esp_err_t guard_err = claw_skill_register_deactivate_guard("cap_lua_run",
                                                               cap_lua_run_deactivate_guard);
    if (guard_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register cap_lua_run deactivate guard: %s",
                 esp_err_to_name(guard_err));
    }

#if BASIC_DEMO_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif
}
