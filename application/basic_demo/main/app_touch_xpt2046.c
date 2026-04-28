/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_touch_xpt2046.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch_xpt2046";

/* XPT2046 control byte layout (start=1, A2..A0, MODE, SER/DFR, PD1, PD0).
 * 12-bit, differential, power down between conversions. */
#define XPT_CMD_X        0xD0      /* measure X (A2A1A0=101, mode=0) */
#define XPT_CMD_Y        0x90      /* measure Y (A2A1A0=001)         */
#define XPT_CMD_Z1       0xB0      /* pressure Z1                    */
#define XPT_CMD_Z2       0xC0      /* pressure Z2                    */

#define TOUCH_POLL_INTERVAL_MS   30
#define TOUCH_PRESSURE_THRESH    400
#define TOUCH_SAMPLE_COUNT       3   /* median filter samples         */
#define TOUCH_SWIPE_MIN_DELTA    30  /* min |dx|/|dy| in px to count as swipe */

typedef struct {
    spi_device_handle_t spi;
    app_touch_xpt2046_config_t cfg;
    app_touch_xpt2046_cb_t cb;
    void *cb_ctx;
    app_touch_xpt2046_gesture_cb_t gesture_cb;
    void *gesture_ctx;
    bool pressed;
    int  start_x;
    int  start_y;
    int  last_x;
    int  last_y;
    int  last_z;
} touch_state_t;

static touch_state_t s_touch;

/* Single 12-bit conversion: write cmd, read 2 bytes back. */
static esp_err_t touch_read_channel(uint8_t cmd, uint16_t *out)
{
    uint8_t tx[3] = { cmd, 0x00, 0x00 };
    uint8_t rx[3] = { 0 };
    spi_transaction_t t = {
        .length = 8 * sizeof(tx),
        .rxlength = 8 * sizeof(rx),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(s_touch.spi, &t);
    if (err != ESP_OK) {
        return err;
    }
    /* Result is 12-bit, MSB first, in rx[1..2] starting from bit 7 of rx[1]. */
    uint16_t raw = ((uint16_t)rx[1] << 8) | rx[2];
    *out = raw >> 3;     /* drop the trailing 3 bits */
    *out &= 0x0FFF;
    return ESP_OK;
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)(*(const uint16_t *)a) - (int)(*(const uint16_t *)b);
}

static esp_err_t touch_read_axis_median(uint8_t cmd, uint16_t *out)
{
    uint16_t s[TOUCH_SAMPLE_COUNT];
    for (int i = 0; i < TOUCH_SAMPLE_COUNT; ++i) {
        esp_err_t err = touch_read_channel(cmd, &s[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    qsort(s, TOUCH_SAMPLE_COUNT, sizeof(s[0]), cmp_u16);
    *out = s[TOUCH_SAMPLE_COUNT / 2];
    return ESP_OK;
}

static int touch_pressure(uint16_t z1, uint16_t z2, uint16_t x_raw)
{
    /* Standard XPT2046 pressure approximation:
     *   R_touch = R_x * (x / 4096) * (z2 / z1 - 1)
     * We don't need physical resistance; the ratio (z2 - z1) inverted
     * versus z1 is enough for a relative threshold. */
    if (z1 == 0) {
        return 0;
    }
    int p = (int)x_raw * ((int)z2 - (int)z1) / (int)z1;
    if (p < 0) {
        p = -p;
    }
    /* A "lighter" press yields a *larger* p in the formula above, so we
     * invert it to a more intuitive monotonic value. */
    int score = 4096 - (p / 8);
    if (score < 0) score = 0;
    return score;
}

static void touch_map_to_screen(uint16_t raw_x, uint16_t raw_y,
                                int *out_x, int *out_y)
{
    const app_touch_xpt2046_cal_t *c = &s_touch.cfg.calibration;
    int sw = s_touch.cfg.screen_width;
    int sh = s_touch.cfg.screen_height;

    int span_x = (int)c->raw_x_max - (int)c->raw_x_min;
    int span_y = (int)c->raw_y_max - (int)c->raw_y_min;
    if (span_x == 0) span_x = 1;
    if (span_y == 0) span_y = 1;

    int rx = (int)raw_x - (int)c->raw_x_min;
    int ry = (int)raw_y - (int)c->raw_y_min;

    /* Map to a square first using each axis's own range. */
    int mx, my;
    if (c->swap_xy) {
        mx = ry * sw / span_y;
        my = rx * sh / span_x;
    } else {
        mx = rx * sw / span_x;
        my = ry * sh / span_y;
    }
    if (c->invert_x) mx = sw - 1 - mx;
    if (c->invert_y) my = sh - 1 - my;

    if (mx < 0) mx = 0;
    if (mx >= sw) mx = sw - 1;
    if (my < 0) my = 0;
    if (my >= sh) my = sh - 1;

    *out_x = mx;
    *out_y = my;
}

static void touch_emit_gesture_on_release(void)
{
    if (!s_touch.gesture_cb) {
        return;
    }
    int dx = s_touch.last_x - s_touch.start_x;
    int dy = s_touch.last_y - s_touch.start_y;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    app_touch_gesture_t g = APP_TOUCH_GESTURE_TAP;
    if (abs_dy >= TOUCH_SWIPE_MIN_DELTA && abs_dy >= abs_dx) {
        g = (dy < 0) ? APP_TOUCH_GESTURE_SWIPE_UP : APP_TOUCH_GESTURE_SWIPE_DOWN;
    } else if (abs_dx >= TOUCH_SWIPE_MIN_DELTA && abs_dx > abs_dy) {
        g = (dx < 0) ? APP_TOUCH_GESTURE_SWIPE_LEFT : APP_TOUCH_GESTURE_SWIPE_RIGHT;
    }
    s_touch.gesture_cb(g, s_touch.start_x, s_touch.start_y,
                       s_touch.last_x, s_touch.last_y, s_touch.gesture_ctx);
}

static void touch_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_INTERVAL_MS));

        uint16_t z1 = 0, z2 = 0, x_raw = 0, y_raw = 0;
        if (touch_read_channel(XPT_CMD_Z1, &z1) != ESP_OK) {
            continue;
        }
        if (z1 < 100) {
            /* No contact -> handle release transition. */
            if (s_touch.pressed) {
                s_touch.pressed = false;
                ESP_LOGI(TAG, "release at (%d,%d)", s_touch.last_x, s_touch.last_y);
                touch_emit_gesture_on_release();
            }
            continue;
        }
        if (touch_read_channel(XPT_CMD_Z2, &z2) != ESP_OK) {
            continue;
        }
        if (touch_read_axis_median(XPT_CMD_X, &x_raw) != ESP_OK) {
            continue;
        }
        if (touch_read_axis_median(XPT_CMD_Y, &y_raw) != ESP_OK) {
            continue;
        }

        int pressure = touch_pressure(z1, z2, x_raw);
        if (pressure < TOUCH_PRESSURE_THRESH) {
            if (s_touch.pressed) {
                s_touch.pressed = false;
                ESP_LOGI(TAG, "release at (%d,%d)", s_touch.last_x, s_touch.last_y);
                touch_emit_gesture_on_release();
            }
            continue;
        }

        int sx = 0, sy = 0;
        touch_map_to_screen(x_raw, y_raw, &sx, &sy);
        s_touch.last_x = sx;
        s_touch.last_y = sy;
        s_touch.last_z = pressure;

        if (!s_touch.pressed) {
            s_touch.pressed = true;
            s_touch.start_x = sx;
            s_touch.start_y = sy;
            ESP_LOGI(TAG, "press   at (%d,%d) p=%d  raw=(%u,%u) z1=%u z2=%u",
                     sx, sy, pressure, x_raw, y_raw, z1, z2);
            if (s_touch.cb) {
                s_touch.cb(sx, sy, pressure, s_touch.cb_ctx);
            }
        } else {
            /* Drag: lighter logging, no callback (avoid log spam). */
            ESP_LOGD(TAG, "drag    at (%d,%d) p=%d", sx, sy, pressure);
        }
    }
}

esp_err_t app_touch_xpt2046_init(const app_touch_xpt2046_config_t *config,
                                 app_touch_xpt2046_cb_t cb,
                                 void *user_ctx)
{
    if (!config || config->cs_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_touch.cfg = *config;
    s_touch.cb = cb;
    s_touch.cb_ctx = user_ctx;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,    /* 1 MHz: XPT2046 max ~2 MHz */
        .mode           = 0,
        .spics_io_num   = config->cs_gpio,
        .queue_size     = 1,
        .flags          = 0,
        .pre_cb         = NULL,
        .post_cb        = NULL,
    };
    esp_err_t err = spi_bus_add_device(config->host, &devcfg, &s_touch.spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Drain the chip into power-down so it doesn't bias the bus on first read. */
    uint16_t dummy;
    (void)touch_read_channel(XPT_CMD_X, &dummy);
    (void)touch_read_channel(XPT_CMD_Y, &dummy);

    BaseType_t ok = xTaskCreate(touch_task, "touch_xpt", 3072, NULL, 2, NULL);
    if (ok != pdPASS) {
        spi_bus_remove_device(s_touch.spi);
        s_touch.spi = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "XPT2046 ready (CS=%d, %dx%d, swap=%d inv_x=%d inv_y=%d)",
             config->cs_gpio,
             config->screen_width, config->screen_height,
             config->calibration.swap_xy,
             config->calibration.invert_x,
             config->calibration.invert_y);
    return ESP_OK;
}

esp_err_t app_touch_xpt2046_set_gesture_callback(app_touch_xpt2046_gesture_cb_t cb,
                                                 void *user_ctx)
{
    s_touch.gesture_cb = cb;
    s_touch.gesture_ctx = user_ctx;
    return ESP_OK;
}
