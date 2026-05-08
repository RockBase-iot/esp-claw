/*
 * SPDX-FileCopyrightText: 2026 RockBase IoT (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_sd_mirror.h"

#include <errno.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include "esp_log.h"

static const char *TAG = "app_sd_mirror";

#define SD_MIRROR_PATH_MAX  256
#define SD_MIRROR_COPY_BUF  2048

/* Optional global SPI bus lock callbacks. When set, every block of SD-side
 * I/O acquires the lock so the polling SDSPI driver cannot interleave with
 * a queued/DMA LCD driver on the same SPI host. See app_sd_mirror.h. */
static app_sd_mirror_bus_lock_fn   s_bus_lock;
static app_sd_mirror_bus_unlock_fn s_bus_unlock;

void app_sd_mirror_set_bus_lock(app_sd_mirror_bus_lock_fn lock,
                                app_sd_mirror_bus_unlock_fn unlock)
{
    s_bus_lock = lock;
    s_bus_unlock = unlock;
}

static inline bool sd_bus_lock(void)
{
    return s_bus_lock ? s_bus_lock() : false;
}

static inline void sd_bus_unlock(bool was_locked)
{
    if (s_bus_unlock) {
        s_bus_unlock(was_locked);
    }
}

/* mkdir -p style helper for VFS-FATFS paths. Tolerates "already exists". */
static esp_err_t ensure_parent_dir(const char *path)
{
    char buf[SD_MIRROR_PATH_MAX];
    size_t len;

    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    len = strnlen(path, sizeof(buf));
    if (len >= sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, path, len + 1);

    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0775) != 0 && errno != EEXIST) {
                ESP_LOGD(TAG, "mkdir(%s) failed: %s", buf, strerror(errno));
                /* keep going — some VFS layers reject root-level mkdir */
            }
            buf[i] = '/';
        }
    }
    return ESP_OK;
}

/* Copy src -> dst byte-for-byte, then propagate mtime so subsequent sweeps
 * see the two sides as identical. Returns ESP_OK on success. */
static esp_err_t copy_file(const char *src, const char *dst, time_t src_mtime)
{
    FILE *in = NULL;
    FILE *out = NULL;
    char *buf = NULL;
    esp_err_t err = ESP_OK;

    ensure_parent_dir(dst);

    in = fopen(src, "rb");
    if (!in) {
        ESP_LOGW(TAG, "open src %s failed: %s", src, strerror(errno));
        return ESP_FAIL;
    }
    out = fopen(dst, "wb");
    if (!out) {
        ESP_LOGW(TAG, "open dst %s failed: %s", dst, strerror(errno));
        fclose(in);
        return ESP_FAIL;
    }
    buf = malloc(SD_MIRROR_COPY_BUF);
    if (!buf) {
        fclose(in);
        fclose(out);
        return ESP_ERR_NO_MEM;
    }

    for (;;) {
        size_t n = fread(buf, 1, SD_MIRROR_COPY_BUF, in);
        if (n == 0) {
            if (ferror(in)) {
                ESP_LOGW(TAG, "read %s failed: %s", src, strerror(errno));
                err = ESP_FAIL;
            }
            break;
        }
        if (fwrite(buf, 1, n, out) != n) {
            ESP_LOGW(TAG, "write %s failed: %s", dst, strerror(errno));
            err = ESP_FAIL;
            break;
        }
    }

    free(buf);
    fclose(in);
    fclose(out);

    if (err == ESP_OK) {
        struct utimbuf times = { .actime = src_mtime, .modtime = src_mtime };
        if (utime(dst, &times) != 0) {
            /* Non-fatal: FATFS sometimes lacks utime; the next sweep will
             * just see the new file as newer and re-copy once. */
            ESP_LOGD(TAG, "utime(%s) failed: %s", dst, strerror(errno));
        }
    }
    return err;
}

static esp_err_t sync_pair(const char *fatfs_path,
                           const char *sd_path,
                           const char *display_label)
{
    struct stat st_fat;
    struct stat st_sd;
    bool have_fat;
    bool have_sd;
    /* The SD-side stat / open / read / write all need to be serialised
     * against any other driver sharing the SPI host (e.g. the LCD). Take
     * the lock around the SD-touching calls only; the FATFS-only calls do
     * not need it. */
    bool spi_locked;

    have_fat = (stat(fatfs_path, &st_fat) == 0) && S_ISREG(st_fat.st_mode);
    spi_locked = sd_bus_lock();
    have_sd  = (stat(sd_path,    &st_sd ) == 0) && S_ISREG(st_sd.st_mode);
    sd_bus_unlock(spi_locked);

    if (!have_fat && !have_sd) {
        ESP_LOGD(TAG, "skip %s (absent on both sides)", display_label);
        return ESP_OK;
    }

    esp_err_t err;
    if (have_fat && !have_sd) {
        ESP_LOGI(TAG, "push  FATFS->SD  %s", display_label);
        spi_locked = sd_bus_lock();
        err = copy_file(fatfs_path, sd_path, st_fat.st_mtime);
        sd_bus_unlock(spi_locked);
        return err;
    }
    if (!have_fat && have_sd) {
        ESP_LOGI(TAG, "pull  SD->FATFS  %s", display_label);
        spi_locked = sd_bus_lock();
        err = copy_file(sd_path, fatfs_path, st_sd.st_mtime);
        sd_bus_unlock(spi_locked);
        return err;
    }

    /* Both exist — newer mtime wins. If they tie, prefer SD as authoritative
     * (the SD copy is the user's persistent data store across re-flashes). */
    if (st_fat.st_mtime > st_sd.st_mtime) {
        ESP_LOGI(TAG, "push  FATFS->SD  %s (fat newer)", display_label);
        spi_locked = sd_bus_lock();
        err = copy_file(fatfs_path, sd_path, st_fat.st_mtime);
        sd_bus_unlock(spi_locked);
        return err;
    } else if (st_sd.st_mtime > st_fat.st_mtime) {
        ESP_LOGI(TAG, "pull  SD->FATFS  %s (sd newer)", display_label);
        spi_locked = sd_bus_lock();
        err = copy_file(sd_path, fatfs_path, st_sd.st_mtime);
        sd_bus_unlock(spi_locked);
        return err;
    }
    ESP_LOGD(TAG, "skip %s (mtimes equal)", display_label);
    return ESP_OK;
}

static esp_err_t sync_one(const char *sd_root, const app_sd_mirror_entry_t *entry)
{
    char sd_path[SD_MIRROR_PATH_MAX];

    if (!entry || !entry->fatfs_path || !entry->sd_relpath) {
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(sd_path, sizeof(sd_path), "%s/%s",
                 sd_root, entry->sd_relpath) >= (int)sizeof(sd_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return sync_pair(entry->fatfs_path, sd_path, entry->sd_relpath);
}

esp_err_t app_sd_mirror_sync(const app_sd_mirror_config_t *config)
{
    size_t failures = 0;
    /* Per-call lock callbacks override the module-global ones for this
     * sweep only; restored before return. */
    app_sd_mirror_bus_lock_fn   saved_lock = s_bus_lock;
    app_sd_mirror_bus_unlock_fn saved_unlock = s_bus_unlock;

    if (!config || !config->sd_root || !config->sd_root[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->entries || config->entry_count == 0) {
        return ESP_OK;
    }

    if (config->bus_lock || config->bus_unlock) {
        s_bus_lock = config->bus_lock;
        s_bus_unlock = config->bus_unlock;
    }

    {
        struct stat st;
        bool spi_locked = sd_bus_lock();
        bool ok = (stat(config->sd_root, &st) == 0) && S_ISDIR(st.st_mode);
        sd_bus_unlock(spi_locked);
        if (!ok) {
            ESP_LOGW(TAG, "SD root %s not available — skipping mirror",
                     config->sd_root);
            s_bus_lock = saved_lock;
            s_bus_unlock = saved_unlock;
            return ESP_ERR_INVALID_STATE;
        }
    }

    for (size_t i = 0; i < config->entry_count; ++i) {
        if (sync_one(config->sd_root, &config->entries[i]) != ESP_OK) {
            ++failures;
        }
    }

    ESP_LOGI(TAG, "sync done: %u entries, %u failures",
             (unsigned)config->entry_count, (unsigned)failures);
    s_bus_lock = saved_lock;
    s_bus_unlock = saved_unlock;
    return ESP_OK;
}

/* Build "<base>/<name>" into out (size cap). Returns false if it overflows. */
static bool join_path(char *out, size_t out_size, const char *base, const char *name)
{
    int n;
    if (!base || !base[0]) {
        n = snprintf(out, out_size, "%s", name);
    } else if (base[strlen(base) - 1] == '/') {
        n = snprintf(out, out_size, "%s%s", base, name);
    } else {
        n = snprintf(out, out_size, "%s/%s", base, name);
    }
    return n > 0 && n < (int)out_size;
}

static bool is_dot_dir(const char *name)
{
    return name && name[0] == '.' &&
           (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

/* True if `dir` already contains a regular file at `relpath`.
 * `dir_on_sd` selects whether the stat() needs the shared SPI bus lock. */
static bool dir_contains_file(const char *dir, const char *relpath, bool dir_on_sd)
{
    char path[SD_MIRROR_PATH_MAX];
    struct stat st;
    if (!join_path(path, sizeof(path), dir, relpath)) {
        return false;
    }
    bool spi_locked = dir_on_sd ? sd_bus_lock() : false;
    int rc = stat(path, &st);
    if (dir_on_sd) {
        sd_bus_unlock(spi_locked);
    }
    return rc == 0 && S_ISREG(st.st_mode);
}

typedef struct {
    char src_dir[SD_MIRROR_PATH_MAX];
    char fatfs_path[SD_MIRROR_PATH_MAX];
    char sd_path[SD_MIRROR_PATH_MAX];
    char rel_path[SD_MIRROR_PATH_MAX];
    char self_path[SD_MIRROR_PATH_MAX];
    char label[SD_MIRROR_PATH_MAX];
} mirror_dir_walk_frame_t;

/* Walk a directory tree; for every regular file, run sync_pair() against the
 * matching file on the other side. When scanning the second side, skip only
 * regular files that already existed on the first side; still recurse into
 * directories so SD-only files inside existing directories can be restored.
 *
 * `src_on_sd` indicates that opendir/readdir/stat against `src_root` need to
 * be wrapped in the shared SPI bus lock. `skip_on_sd` does the same for the
 * skip_root probe.
 */
static size_t mirror_dir_side_recursive(const char *src_root,
                                        const char *fatfs_root,
                                        const char *sd_root,
                                        const char *rel_dir,
                                        const char *display_prefix,
                                        const char *skip_root,
                                        bool src_on_sd,
                                        bool skip_on_sd)
{
    DIR *dh;
    struct dirent *ent;
    mirror_dir_walk_frame_t *frame = NULL;
    size_t failures = 0;
    bool spi_locked;

    frame = calloc(1, sizeof(*frame));
    if (!frame) {
        return 1;
    }

    if (rel_dir && rel_dir[0]) {
        if (!join_path(frame->src_dir, sizeof(frame->src_dir), src_root, rel_dir)) {
            free(frame);
            return 1;
        }
    } else if (snprintf(frame->src_dir, sizeof(frame->src_dir), "%s", src_root) >= (int)sizeof(frame->src_dir)) {
        free(frame);
        return 1;
    }

    spi_locked = src_on_sd ? sd_bus_lock() : false;
    dh = opendir(frame->src_dir);
    if (!dh) {
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "opendir(%s) failed: %s", frame->src_dir, strerror(errno));
        }
        if (src_on_sd) {
            sd_bus_unlock(spi_locked);
        }
        free(frame);
        return 0;
    }

    while ((ent = readdir(dh)) != NULL) {
        struct stat st;

        if (is_dot_dir(ent->d_name)) {
            continue;
        }

        if (rel_dir && rel_dir[0]) {
            if (!join_path(frame->rel_path, sizeof(frame->rel_path), rel_dir, ent->d_name)) {
                ++failures;
                continue;
            }
        } else if (snprintf(frame->rel_path, sizeof(frame->rel_path), "%s", ent->d_name) >= (int)sizeof(frame->rel_path)) {
            ++failures;
            continue;
        }

        if (!join_path(frame->self_path, sizeof(frame->self_path), frame->src_dir, ent->d_name)) {
            ++failures;
            continue;
        }
        if (stat(frame->self_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            /* Recurse — release the lock so the recursive call can re-take it
             * (the arbiter mutex is recursive, but releasing here lets LCD
             * draws interleave between subdirectories). */
            if (src_on_sd) {
                sd_bus_unlock(spi_locked);
            }
            failures += mirror_dir_side_recursive(src_root,
                                                  fatfs_root,
                                                  sd_root,
                                                  frame->rel_path,
                                                  display_prefix,
                                                  skip_root,
                                                  src_on_sd,
                                                  skip_on_sd);
            spi_locked = src_on_sd ? sd_bus_lock() : false;
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        /* Release the src-side SPI lock around the per-file sync_pair() call
         * so it can take the lock with its own (smaller) granularity around
         * just the SD-side stat / read / write blocks. Re-acquire afterwards
         * to continue the readdir loop on this directory. */
        if (src_on_sd) {
            sd_bus_unlock(spi_locked);
        }

        bool skip = false;
        if (skip_root && dir_contains_file(skip_root, frame->rel_path, skip_on_sd)) {
            skip = true;
        }

        if (!skip) {
            /* Build both candidate paths regardless of which side we are scanning. */
            if (!join_path(frame->fatfs_path, sizeof(frame->fatfs_path), fatfs_root, frame->rel_path) ||
                    !join_path(frame->sd_path, sizeof(frame->sd_path), sd_root, frame->rel_path)) {
                ESP_LOGW(TAG, "path too long for %s", frame->rel_path);
                ++failures;
            } else {
                if (display_prefix && display_prefix[0]) {
                    int wrote = snprintf(frame->label, sizeof(frame->label), "%.80s/%.170s",
                                         display_prefix, frame->rel_path);
                    (void)wrote;
                } else {
                    int wrote = snprintf(frame->label, sizeof(frame->label), "%.250s", frame->rel_path);
                    (void)wrote;
                }

                if (sync_pair(frame->fatfs_path, frame->sd_path, frame->label) != ESP_OK) {
                    ++failures;
                }
            }
        }

        spi_locked = src_on_sd ? sd_bus_lock() : false;
    }
    closedir(dh);
    if (src_on_sd) {
        sd_bus_unlock(spi_locked);
    }
    free(frame);
    return failures;
}

esp_err_t app_sd_mirror_sync_dir(const char *sd_root,
                                 const char *fatfs_dir,
                                 const char *sd_subdir)
{
    char *sd_dir = NULL;
    struct stat st;
    size_t failures = 0;
    esp_err_t err = ESP_OK;
    bool spi_locked;

    if (!sd_root || !sd_root[0] || !fatfs_dir || !fatfs_dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_locked = sd_bus_lock();
    bool sd_root_ok = (stat(sd_root, &st) == 0) && S_ISDIR(st.st_mode);
    sd_bus_unlock(spi_locked);
    if (!sd_root_ok) {
        ESP_LOGW(TAG, "SD root %s not available — skipping dir mirror %s",
                 sd_root, fatfs_dir);
        return ESP_ERR_INVALID_STATE;
    }

    sd_dir = calloc(1, SD_MIRROR_PATH_MAX);
    if (!sd_dir) {
        return ESP_ERR_NO_MEM;
    }

    if (sd_subdir && sd_subdir[0]) {
        if (!join_path(sd_dir, SD_MIRROR_PATH_MAX, sd_root, sd_subdir)) {
            err = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
    } else {
        if (snprintf(sd_dir, SD_MIRROR_PATH_MAX, "%s", sd_root) >= SD_MIRROR_PATH_MAX) {
            err = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
    }

    /* Make sure the SD destination dir exists (with a trailing component so
     * ensure_parent_dir creates it). */
    {
        char *probe = calloc(1, SD_MIRROR_PATH_MAX);
        if (!probe) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        if (snprintf(probe, SD_MIRROR_PATH_MAX, "%s/.keep", sd_dir) < SD_MIRROR_PATH_MAX) {
            bool spi_locked = sd_bus_lock();
            (void)ensure_parent_dir(probe);
            sd_bus_unlock(spi_locked);
        }
        free(probe);
    }

    /* Pass 1: walk FATFS -> mirror to SD. (src is FATFS, skip_root is SD.) */
    failures += mirror_dir_side_recursive(fatfs_dir, fatfs_dir, sd_dir, "",
                                          sd_subdir, NULL,
                                          /*src_on_sd=*/false,
                                          /*skip_on_sd=*/true);
    /* Pass 2: walk SD -> mirror back any files that were missing on FATFS,
     * skipping anything already processed in pass 1. (src is SD, skip_root
     * is FATFS.) */
    failures += mirror_dir_side_recursive(sd_dir, fatfs_dir, sd_dir, "",
                                          sd_subdir, fatfs_dir,
                                          /*src_on_sd=*/true,
                                          /*skip_on_sd=*/false);

    ESP_LOGI(TAG, "dir sync done: %s <-> %s (%u failures)",
             fatfs_dir, sd_dir, (unsigned)failures);
cleanup:
    free(sd_dir);
    return err;
}
