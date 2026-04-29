/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdcard_mount.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dev_fs_fat.h"
#include "esp_board_manager.h"
#include "esp_board_periph.h"
#include "periph_spi.h"
#include "spi_bus_arbiter.h"

#define SDCARD_DEVICE_NAME "fs_sdcard"

/*
 * Workaround for esp_board_manager v0.5.x: both `dev_fs_fat_sub_spi.c` and
 * `dev_display_lcd_sub_spi.c` register a sub-device entry named "spi" via
 * ESP_BOARD_ENTRY_IMPLEMENT(spi, ...). The lookup in
 * `esp_board_entry_find_desc()` is a linear scan over the linker section,
 * so whichever was linked first wins — for our boards that is the LCD
 * driver. As a result `esp_board_manager_init_device_by_name("fs_sdcard")`
 * dispatches into the LCD SPI initializer and bails out.
 *
 * To avoid patching the managed component we drive `esp_vfs_fat_sdspi_mount`
 * ourselves (see `sdcard_try_mount` below) and manage the SPI peripheral
 * refcount via `esp_board_periph_get_handle`/`_unref_handle`. This also lets
 * us inspect the precise error code from the mount attempt and short-circuit
 * pointless retries when the failure is purely filesystem-level (exFAT/NTFS).
 */

static const char *TAG = "sdcard_mount";

static bool s_mounted;
static char s_mount_point[32];
static dev_fs_fat_handle_t *s_fs_handle;

/* Frequencies (in kHz) attempted in order. The SD spec requires init at
 * <=400 kHz; any "fast" frequency configured in the board YAML is only used
 * after the card is in TRAN state. With a shared SPI bus and no external
 * MISO pull-up the high-speed retry often fails CRC checks, so we fall back
 * to progressively safer rates. */
static const uint32_t s_freq_kHz_ladder[] = { 0, 4000, 1000 };

/**
 * @brief  Apply internal pull-ups to MISO/MOSI/CS so the bus is well defined
 *         before the first CMD0. This compensates for boards (like the
 *         NM-CYD-C5) that omit the SD-spec-mandated 10-50 kΩ pull-ups.
 *         SCLK is left alone — it is actively driven by the SPI peripheral.
 */
static void apply_sd_bus_pullups(const dev_fs_fat_config_t *cfg)
{
    int cs_gpio = cfg->sub_cfg.spi.cs_gpio_num;
    if (cs_gpio >= 0) {
        gpio_set_pull_mode((gpio_num_t)cs_gpio, GPIO_PULLUP_ONLY);
    }

    const char *bus_name = cfg->sub_cfg.spi.spi_bus_name;
    if (!bus_name || !bus_name[0]) {
        return;
    }
    void *p_cfg = NULL;
    if (esp_board_manager_get_periph_config(bus_name, &p_cfg) != ESP_OK || !p_cfg) {
        return;
    }
    const periph_spi_config_t *spi_cfg = (const periph_spi_config_t *)p_cfg;
    int miso = spi_cfg->spi_bus_config.miso_io_num;
    int mosi = spi_cfg->spi_bus_config.mosi_io_num;
    int sclk = spi_cfg->spi_bus_config.sclk_io_num;
    if (miso >= 0) {
        gpio_set_pull_mode((gpio_num_t)miso, GPIO_PULLUP_ONLY);
    }
    if (mosi >= 0) {
        gpio_set_pull_mode((gpio_num_t)mosi, GPIO_PULLUP_ONLY);
    }
    ESP_LOGI(TAG, "applied internal pull-ups: cs=%d miso=%d mosi=%d (sclk=%d, driven)",
             cs_gpio, miso, mosi, sclk);
}

/**
 * @brief  Perform the SDSPI host setup + FATFS mount in one call so we can
 *         distinguish low-level signal failures from filesystem failures.
 *
 *         The managed component's `dev_fs_fat_sub_spi_init()` collapses both
 *         classes of error into rc=-1, which prevents us from short-circuiting
 *         retries when we already know the card is fine but its filesystem
 *         is unreadable (e.g. exFAT on an SDXC card).
 *
 * @return
 *  - ESP_OK on success.
 *  - ESP_ERR_INVALID_RESPONSE / ESP_ERR_INVALID_CRC / ESP_ERR_TIMEOUT etc.
 *    when sdmmc_card_init() failed (signal-level / no-card).
 *  - ESP_FAIL when sdmmc_card_init() succeeded but f_mount() rejected the
 *    on-card filesystem (= unsupported FS such as exFAT/NTFS/corrupt).
 */
static esp_err_t sdcard_try_mount(const dev_fs_fat_config_t *cfg, void **out_handle)
{
    *out_handle = NULL;

    periph_spi_handle_t *spi_handle = NULL;
    const char *bus_name = cfg->sub_cfg.spi.spi_bus_name;
    if (!bus_name || !bus_name[0]) {
        ESP_LOGE(TAG, "spi_bus_name is empty");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_board_periph_get_handle(bus_name, (void **)&spi_handle);
    if (err != ESP_OK || !spi_handle) {
        ESP_LOGE(TAG, "failed to get SPI peripheral handle '%s': %s",
                 bus_name, esp_err_to_name(err));
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    dev_fs_fat_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) {
        esp_board_periph_unref_handle(bus_name);
        return ESP_ERR_NO_MEM;
    }
    handle->host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    handle->host.max_freq_khz = cfg->frequency;
    handle->host.slot = spi_handle->spi_port;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = cfg->sub_cfg.spi.cs_gpio_num;
    slot.host_id = handle->host.slot;

    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = cfg->vfs_config.format_if_mount_failed,
        .max_files = cfg->vfs_config.max_files,
        .allocation_unit_size = cfg->vfs_config.allocation_unit_size,
    };

    /* Take the shared SPI bus lock for the duration of the SD initialisation
     * handshake so the LCD/touch drivers on the same host cannot start a
     * transaction halfway through a CMDx exchange. The lock auto-creates on
     * first use; if it cannot be created we proceed unprotected (better
     * than refusing to mount altogether). */
    bool spi_locked = (spi_bus_arbiter_lock(2000) == ESP_OK);
    err = esp_vfs_fat_sdspi_mount(cfg->mount_point, &handle->host, &slot, &mount, &handle->card);
    if (spi_locked) {
        spi_bus_arbiter_unlock();
    }
    if (err != ESP_OK) {
        esp_board_periph_unref_handle(bus_name);
        free(handle);
        return err;
    }

    handle->mount_point = (char *)cfg->mount_point;
    *out_handle = handle;
    return ESP_OK;
}

bool sdcard_mount_is_mounted(void)
{
    return s_mounted;
}

const char *sdcard_mount_get_mount_point(void)
{
    return s_mounted ? s_mount_point : NULL;
}

esp_err_t sdcard_mount_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    /* Probe whether the board has an `fs_sdcard` entry at all. If not, this
     * board simply has no SD slot — that is not an error, just nothing to do.
     */
    void *cfg_void = NULL;
    esp_err_t err = esp_board_manager_get_device_config(SDCARD_DEVICE_NAME,
                                                        &cfg_void);
    if (err != ESP_OK || cfg_void == NULL) {
        ESP_LOGI(TAG, "no '%s' device on this board (%s)",
                 SDCARD_DEVICE_NAME, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    const dev_fs_fat_config_t *cfg = (const dev_fs_fat_config_t *)cfg_void;
    const char *mp = (cfg->mount_point && cfg->mount_point[0])
                     ? cfg->mount_point : "/sdcard";
    const char *sub_type = cfg->sub_type ? cfg->sub_type : "(null)";
    const char *bus_name = cfg->sub_cfg.spi.spi_bus_name
                           ? cfg->sub_cfg.spi.spi_bus_name : "(null)";
    ESP_LOGI(TAG, "fs_sdcard cfg: mount=%s sub_type=%s spi_bus=%s cs_gpio=%d freq_khz=%d",
             mp, sub_type, bus_name, cfg->sub_cfg.spi.cs_gpio_num,
             (int)cfg->frequency);

    /* Direct dispatch — see the comment above. We only support sub_type "spi"
     * here; that is the only flavour the NM-CYD boards expose. Boards with an
     * SDMMC slot are unaffected by the entry-name collision because the
     * "sdmmc" name is unique. */
    if (!cfg->sub_type || strcmp(cfg->sub_type, "spi") != 0) {
        ESP_LOGW(TAG, "unsupported sub_type '%s' (expected 'spi')",
                 sub_type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    void *raw_handle = NULL;
    int rc = -1;
    /* Apply internal pull-ups once before any CMD is issued. SD spec
     * requires pull-ups on MISO/MOSI/CS in SPI mode; without them the
     * R1 response from the card is read while MISO is still floating
     * and we get spurious CRC errors on CMD5/CMD52/CMD59. */
    apply_sd_bus_pullups(cfg);

    /* Power-up settling: SD spec requires the card to see Vdd stable for
     * >=1 ms and at least 74 dummy clocks (with CS deasserted) before the
     * first command. When the SDSPI driver issues CMD0 too soon after
     * boot some cards (esp. older 4 GB SDSC) reply with garbage that the
     * host interprets as a CRC error. A short delay before the first
     * attempt gives the card time to come up. */
    int cs_gpio = cfg->sub_cfg.spi.cs_gpio_num;
    if (cs_gpio >= 0) {
        gpio_set_direction((gpio_num_t)cs_gpio, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)cs_gpio, 1); /* deassert CS */
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Make a stack copy so we can lower `frequency` on later attempts
     * without mutating the board manager's owned config. */
    dev_fs_fat_config_t cfg_local = *cfg;

    bool fs_unsupported = false;
    const int n_attempts = sizeof(s_freq_kHz_ladder) / sizeof(s_freq_kHz_ladder[0]);
    for (int attempt = 0; attempt < n_attempts; ++attempt) {
        uint32_t freq_override = s_freq_kHz_ladder[attempt];
        cfg_local.frequency = (freq_override != 0) ? freq_override : cfg->frequency;
        ESP_LOGI(TAG, "SD mount attempt %d/%d at %u kHz",
                 attempt + 1, n_attempts, (unsigned)cfg_local.frequency);
        raw_handle = NULL;
        esp_err_t mount_err = sdcard_try_mount(&cfg_local, &raw_handle);
        if (mount_err == ESP_OK && raw_handle != NULL) {
            rc = 0;
            break;
        }
        rc = -1;
        ESP_LOGW(TAG, "SD mount attempt %d/%d failed: %s (0x%x)",
                 attempt + 1, n_attempts, esp_err_to_name(mount_err), mount_err);
        /* try_mount() frees its own handle and unrefs the SPI peripheral on
         * failure, so raw_handle is already invalid here — do not touch it. */
        raw_handle = NULL;
        if (mount_err == ESP_FAIL || mount_err == ESP_ERR_NOT_SUPPORTED) {
            /* Card responded — sdmmc_card_init() succeeded — but f_mount()
             * could not parse the partition table / BPB. Lowering the SPI
             * clock will not help; the filesystem is simply unsupported
             * (NTFS, exFAT-formatted SDXC, or corrupt). Stop retrying. */
            fs_unsupported = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (rc != 0 || raw_handle == NULL) {
        if (fs_unsupported) {
            ESP_LOGE(TAG, "SD card detected at %s but FAT mount failed.",
                     mp);
            ESP_LOGE(TAG, "Cause: unsupported filesystem.");
            ESP_LOGE(TAG, "  Supported: FAT12 / FAT16 / FAT32.");
            ESP_LOGE(TAG, "  NOT supported: exFAT (SDXC default), NTFS, ext*.");
            ESP_LOGE(TAG, "  -> Reformat the card as FAT32.");
            ESP_LOGE(TAG, "  -> Cards >32 GB ship pre-formatted as exFAT;");
            ESP_LOGE(TAG, "     use a tool such as guiformat / mkfs.fat -F32.");
        } else {
            ESP_LOGW(TAG, "SD card mount failed at %s. Possible causes:", mp);
            ESP_LOGW(TAG, "  * no card inserted");
            ESP_LOGW(TAG, "  * weak signal on shared SPI (MISO pull-up, wiring)");
            ESP_LOGW(TAG, "  * card is bad / locked");
        }
        return ESP_FAIL;
    }
    s_fs_handle = (dev_fs_fat_handle_t *)raw_handle;
    strlcpy(s_mount_point, mp, sizeof(s_mount_point));
    s_mounted = true;

    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(s_mount_point, &total, &freeb) == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at %s, total=%llu KiB, free=%llu KiB",
                 s_mount_point,
                 (unsigned long long)(total / 1024),
                 (unsigned long long)(freeb / 1024));
    } else {
        ESP_LOGI(TAG, "SD card mounted at %s", s_mount_point);
    }
    return ESP_OK;
}

esp_err_t sdcard_mount_get_usage(uint64_t *out_total_bytes,
                                 uint64_t *out_free_bytes)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t total = 0, freeb = 0;
    esp_err_t err = esp_vfs_fat_info(s_mount_point, &total, &freeb);
    if (err != ESP_OK) {
        return err;
    }
    if (out_total_bytes) {
        *out_total_bytes = total;
    }
    if (out_free_bytes) {
        *out_free_bytes = freeb;
    }
    return ESP_OK;
}

void sdcard_mount_force_unmount(void)
{
    if (!s_mounted) {
        return;
    }
    ESP_LOGW(TAG, "force-unmounting SD card at %s", s_mount_point);
    if (s_fs_handle) {
        bool spi_locked = (spi_bus_arbiter_lock(2000) == ESP_OK);
        if (s_fs_handle->mount_point && s_fs_handle->card) {
            esp_vfs_fat_sdcard_unmount(s_fs_handle->mount_point, s_fs_handle->card);
        }
        if (spi_locked) {
            spi_bus_arbiter_unlock();
        }
        /* Drop our reference on the shared SPI bus that try_mount() acquired. */
        void *cfg_void = NULL;
        if (esp_board_manager_get_device_config(SDCARD_DEVICE_NAME, &cfg_void) == ESP_OK
                && cfg_void) {
            const dev_fs_fat_config_t *cfg = (const dev_fs_fat_config_t *)cfg_void;
            if (cfg->sub_cfg.spi.spi_bus_name && cfg->sub_cfg.spi.spi_bus_name[0]) {
                esp_board_periph_unref_handle(cfg->sub_cfg.spi.spi_bus_name);
            }
        }
        free(s_fs_handle);
        s_fs_handle = NULL;
    }
    s_mounted = false;
    s_mount_point[0] = '\0';
}

bool sdcard_mount_check_alive(void)
{
    if (!s_mounted) {
        return false;
    }
    /* esp_vfs_fat_info() issues a FAT free-cluster scan, which on SDSPI
     * boils down to a small CMD17 read. If the card has been pulled the
     * SDMMC layer returns ESP_ERR_INVALID_CRC (0x107) almost immediately. */
    uint64_t total = 0, freeb = 0;
    bool spi_locked = (spi_bus_arbiter_lock(2000) == ESP_OK);
    esp_err_t err = esp_vfs_fat_info(s_mount_point, &total, &freeb);
    if (spi_locked) {
        spi_bus_arbiter_unlock();
    }
    if (err == ESP_OK) {
        return true;
    }
    ESP_LOGW(TAG, "SD probe failed (%s) — assuming card removed",
             esp_err_to_name(err));
    sdcard_mount_force_unmount();
    return false;
}

/* Cool-down (in xTaskGetTickCount() ticks) between consecutive remount
 * attempts. The web UI calls into the alive_cb on every page refresh, and
 * each failed mount attempt now spends ~600 ms hammering the SDSPI bus,
 * which would noticeably stutter the UI if the user clicks Refresh
 * repeatedly with no card inserted. 5 s feels snappy without being abusive.
 */
#define SDCARD_REMOUNT_COOLDOWN_MS 5000

bool sdcard_mount_alive_or_remount(void)
{
    if (s_mounted) {
        return sdcard_mount_check_alive();
    }
    static TickType_t s_last_attempt_tick;
    static bool s_have_attempted;
    TickType_t now = xTaskGetTickCount();
    if (s_have_attempted) {
        TickType_t elapsed = now - s_last_attempt_tick;
        if (elapsed < pdMS_TO_TICKS(SDCARD_REMOUNT_COOLDOWN_MS)) {
            return false;
        }
    }
    s_have_attempted = true;
    s_last_attempt_tick = now;
    ESP_LOGI(TAG, "probing for SD card insertion…");
    esp_err_t err = sdcard_mount_init();
    return err == ESP_OK;
}
