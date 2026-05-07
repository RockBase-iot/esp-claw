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

#define SD_MIRROR_PATH_MAX  192
#define SD_MIRROR_COPY_BUF  2048

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

    have_fat = (stat(fatfs_path, &st_fat) == 0) && S_ISREG(st_fat.st_mode);
    have_sd  = (stat(sd_path,    &st_sd ) == 0) && S_ISREG(st_sd.st_mode);

    if (!have_fat && !have_sd) {
        ESP_LOGD(TAG, "skip %s (absent on both sides)", display_label);
        return ESP_OK;
    }

    if (have_fat && !have_sd) {
        ESP_LOGI(TAG, "push  FATFS->SD  %s", display_label);
        return copy_file(fatfs_path, sd_path, st_fat.st_mtime);
    }
    if (!have_fat && have_sd) {
        ESP_LOGI(TAG, "pull  SD->FATFS  %s", display_label);
        return copy_file(sd_path, fatfs_path, st_sd.st_mtime);
    }

    /* Both exist — newer mtime wins. If they tie, prefer SD as authoritative
     * (the SD copy is the user's persistent data store across re-flashes). */
    if (st_fat.st_mtime > st_sd.st_mtime) {
        ESP_LOGI(TAG, "push  FATFS->SD  %s (fat newer)", display_label);
        return copy_file(fatfs_path, sd_path, st_fat.st_mtime);
    } else if (st_sd.st_mtime > st_fat.st_mtime) {
        ESP_LOGI(TAG, "pull  SD->FATFS  %s (sd newer)", display_label);
        return copy_file(sd_path, fatfs_path, st_sd.st_mtime);
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

    if (!config || !config->sd_root || !config->sd_root[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->entries || config->entry_count == 0) {
        return ESP_OK;
    }

    {
        struct stat st;
        if (stat(config->sd_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ESP_LOGW(TAG, "SD root %s not available — skipping mirror",
                     config->sd_root);
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

/* True if `dir` already contains an entry equal to `name` (regular file). */
static bool dir_contains_file(const char *dir, const char *name)
{
    char path[SD_MIRROR_PATH_MAX];
    struct stat st;
    if (!join_path(path, sizeof(path), dir, name)) {
        return false;
    }
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Walk dir; for every regular file, run sync_pair() against the matching
 * file on the other side (which may or may not exist). Skips entries
 * already processed via the `processed_dir` cross-check (used to avoid
 * doing the same pair twice when scanning the second side). */
static size_t mirror_dir_side(const char *src_dir,
                              const char *dst_dir,
                              const char *fatfs_dir,
                              const char *sd_dir,
                              const char *display_prefix,
                              const char *skip_dir)
{
    DIR *dh;
    struct dirent *ent;
    char fatfs_path[SD_MIRROR_PATH_MAX];
    char sd_path[SD_MIRROR_PATH_MAX];
    char label[SD_MIRROR_PATH_MAX];
    size_t failures = 0;

    (void)dst_dir;
    dh = opendir(src_dir);
    if (!dh) {
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "opendir(%s) failed: %s", src_dir, strerror(errno));
        }
        return 0;
    }

    while ((ent = readdir(dh)) != NULL) {
        if (ent->d_name[0] == '.' &&
                (ent->d_name[1] == '\0' ||
                 (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }

        /* Build both candidate paths regardless of which side we are scanning. */
        if (!join_path(fatfs_path, sizeof(fatfs_path), fatfs_dir, ent->d_name) ||
                !join_path(sd_path, sizeof(sd_path), sd_dir, ent->d_name)) {
            ESP_LOGW(TAG, "path too long under %s/%s", src_dir, ent->d_name);
            ++failures;
            continue;
        }

        /* When scanning the second side, skip files that already existed on
         * the first side (they were handled there). */
        if (skip_dir && dir_contains_file(skip_dir, ent->d_name)) {
            continue;
        }

        /* Skip subdirectories — only regular files are mirrored. */
        {
            char self_path[SD_MIRROR_PATH_MAX];
            struct stat st;
            if (!join_path(self_path, sizeof(self_path), src_dir, ent->d_name)) {
                ++failures;
                continue;
            }
            if (stat(self_path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
        }

        if (display_prefix && display_prefix[0]) {
            int wrote = snprintf(label, sizeof(label), "%.80s/%.100s",
                                 display_prefix, ent->d_name);
            (void)wrote;
        } else {
            int wrote = snprintf(label, sizeof(label), "%.180s", ent->d_name);
            (void)wrote;
        }

        if (sync_pair(fatfs_path, sd_path, label) != ESP_OK) {
            ++failures;
        }
    }
    closedir(dh);
    return failures;
}

esp_err_t app_sd_mirror_sync_dir(const char *sd_root,
                                 const char *fatfs_dir,
                                 const char *sd_subdir)
{
    char sd_dir[SD_MIRROR_PATH_MAX];
    struct stat st;
    size_t failures = 0;

    if (!sd_root || !sd_root[0] || !fatfs_dir || !fatfs_dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(sd_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "SD root %s not available — skipping dir mirror %s",
                 sd_root, fatfs_dir);
        return ESP_ERR_INVALID_STATE;
    }

    if (sd_subdir && sd_subdir[0]) {
        if (!join_path(sd_dir, sizeof(sd_dir), sd_root, sd_subdir)) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        if (snprintf(sd_dir, sizeof(sd_dir), "%s", sd_root) >= (int)sizeof(sd_dir)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    /* Make sure the SD destination dir exists (with a trailing component so
     * ensure_parent_dir creates it). */
    {
        char probe[SD_MIRROR_PATH_MAX];
        if (snprintf(probe, sizeof(probe), "%s/.keep", sd_dir) < (int)sizeof(probe)) {
            ensure_parent_dir(probe);
        }
    }

    /* Pass 1: walk FATFS -> mirror to SD. */
    failures += mirror_dir_side(fatfs_dir, sd_dir, fatfs_dir, sd_dir,
                                sd_subdir, NULL);
    /* Pass 2: walk SD -> mirror back any files that were missing on FATFS,
     * skipping anything already processed in pass 1. */
    failures += mirror_dir_side(sd_dir, fatfs_dir, fatfs_dir, sd_dir,
                                sd_subdir, fatfs_dir);

    ESP_LOGI(TAG, "dir sync done: %s <-> %s (%u failures)",
             fatfs_dir, sd_dir, (unsigned)failures);
    return ESP_OK;
}
