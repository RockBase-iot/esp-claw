/*
 * SPDX-FileCopyrightText: 2026 RockBase IoT (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_boot_button.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "boot_btn";

typedef struct {
    int gpio;
    app_boot_button_press_cb_t cb;
    void *cb_ctx;
    app_boot_button_press_cb_t long_cb;
    void *long_cb_ctx;
    uint32_t long_press_ms;
} app_boot_button_state_t;

static app_boot_button_state_t s_btn;

#define BOOT_POLL_INTERVAL_MS   25
#define BOOT_DEBOUNCE_SAMPLES   2

static void app_boot_button_task(void *arg)
{
    (void)arg;
    int last_stable = 1;          /* idle high (pull-up, pressed = low) */
    int debounce_count = 0;
    int last_sample = 1;
    TickType_t press_tick = 0;
    bool long_fired = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_INTERVAL_MS));
        int level = gpio_get_level(s_btn.gpio);
        if (level == last_sample) {
            if (debounce_count < BOOT_DEBOUNCE_SAMPLES) {
                debounce_count++;
            }
            if (debounce_count >= BOOT_DEBOUNCE_SAMPLES && level != last_stable) {
                /* Edge detected, stable. */
                if (last_stable == 1 && level == 0) {
                    /* Falling edge -> press begins. */
                    press_tick = xTaskGetTickCount();
                    long_fired = false;
                } else if (last_stable == 0 && level == 1) {
                    /* Rising edge -> release. Deliver the short-press only if a
                     * long press was not already fired during this hold. */
                    if (!long_fired && s_btn.cb) {
                        s_btn.cb(s_btn.cb_ctx);
                    }
                }
                last_stable = level;
            }
        } else {
            last_sample = level;
            debounce_count = 0;
        }

        /* Long-press detection while the (stable) button is held down. */
        if (last_stable == 0 && !long_fired && s_btn.long_cb &&
                s_btn.long_press_ms > 0) {
            uint32_t held_ms =
                (uint32_t)(xTaskGetTickCount() - press_tick) * portTICK_PERIOD_MS;
            if (held_ms >= s_btn.long_press_ms) {
                long_fired = true;
                s_btn.long_cb(s_btn.long_cb_ctx);
            }
        }
    }
}

esp_err_t app_boot_button_init(int gpio_num,
                               app_boot_button_press_cb_t cb,
                               void *user_ctx)
{
    if (gpio_num < 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_IDF_TARGET_ESP32S3
    /* On ESP32-S3 modules with in-package octal flash/PSRAM, GPIO 26-37 are
     * the SPI0/1 pins wired to the flash and PSRAM. Reconfiguring them at
     * runtime (especially with CONFIG_SPIRAM_XIP_FROM_PSRAM enabled) hangs the
     * CPU and causes a TG1WDT watchdog reset. Refuse such pins outright. */
    if (gpio_num >= 26 && gpio_num <= 37) {
        ESP_LOGE(TAG, "GPIO%d is a flash/PSRAM pin, refusing to use as BOOT button", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }
#endif
    s_btn.gpio = gpio_num;
    s_btn.cb = cb;
    s_btn.cb_ctx = user_ctx;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(%d) failed: %s", gpio_num, esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(app_boot_button_task, "boot_btn",
                                6144, NULL, 2, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BOOT button on GPIO%d active-low ready", gpio_num);
    return ESP_OK;
}

esp_err_t app_boot_button_set_long_press(uint32_t duration_ms,
                                         app_boot_button_press_cb_t cb,
                                         void *user_ctx)
{
    s_btn.long_press_ms = cb ? duration_ms : 0;
    s_btn.long_cb = cb;
    s_btn.long_cb_ctx = user_ctx;
    ESP_LOGI(TAG, "BOOT long-press %s (%u ms)",
             cb ? "enabled" : "disabled", (unsigned)duration_ms);
    return ESP_OK;
}
