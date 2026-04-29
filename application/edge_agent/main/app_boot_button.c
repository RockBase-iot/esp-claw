/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_boot_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "boot_btn";

typedef struct {
    int gpio;
    app_boot_button_press_cb_t cb;
    void *cb_ctx;
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
                    /* Falling edge -> press. */
                    if (s_btn.cb) {
                        s_btn.cb(s_btn.cb_ctx);
                    }
                }
                last_stable = level;
            }
        } else {
            last_sample = level;
            debounce_count = 0;
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
                                2048, NULL, 2, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BOOT button on GPIO%d active-low ready", gpio_num);
    return ESP_OK;
}
