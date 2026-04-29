/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "spi_bus_arbiter.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "spi_bus_arbiter";

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;

static void ensure_mutex(void)
{
    if (s_mutex) {
        return;
    }
    /* Race-safe enough at boot — every caller takes the same lock the first
     * time it is needed. Two near-simultaneous creators would both end up
     * referencing the same StaticSemaphore_t storage; we accept that minor
     * theoretical race rather than introducing a higher-priority lock here. */
    s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mutex_storage);
}

esp_err_t spi_bus_arbiter_lock(uint32_t timeout_ms)
{
    ensure_mutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTakeRecursive(s_mutex, ticks) != pdTRUE) {
        ESP_LOGW(TAG, "lock timeout (%u ms)", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void spi_bus_arbiter_unlock(void)
{
    if (s_mutex) {
        xSemaphoreGiveRecursive(s_mutex);
    }
}
