/*
 * SPDX-FileCopyrightText: 2026 RockBase-iot Technology (Chengdu) CO., LTD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Compile the nanosvg implementation into this translation unit by
 * defining the implementation guards before including the headers.
 */
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION

#include "nanosvg.h"
#include "nanosvgrast.h"

#include "nanosvg_wrapper.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "nanosvg_wrapper";

/* Maximum SVG file size we will read into memory (1 MiB). */
#define NANOSVG_MAX_FILE_SIZE (1024 * 1024)

static char *read_file_to_string(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "failed to open %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len < 0 || (size_t)len > NANOSVG_MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "SVG file too large: %ld bytes", len);
        fclose(f);
        return NULL;
    }

    /* +1 for null terminator */
    char *buf = (char *)heap_caps_malloc((size_t)len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        /* Fallback to internal RAM */
        buf = (char *)malloc((size_t)len + 1);
    }
    if (!buf) {
        ESP_LOGE(TAG, "failed to allocate %ld bytes for SVG", len);
        fclose(f);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    fclose(f);

    if (read_bytes != (size_t)len) {
        ESP_LOGE(TAG, "short read: expected %ld, got %u", len, (unsigned)read_bytes);
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    *out_len = (size_t)len;
    return buf;
}

/**
 * @brief  Convert RGBA (non-premultiplied) to RGB565 with optional byte swap.
 *
 * Alpha is composited over a black background.
 */
static void rgba_to_rgb565(const unsigned char *rgba,
                            uint16_t *rgb565,
                            int pixel_count,
                            bool swap_bytes)
{
    for (int i = 0; i < pixel_count; i++) {
        unsigned char r = rgba[i * 4 + 0];
        unsigned char g = rgba[i * 4 + 1];
        unsigned char b = rgba[i * 4 + 2];
        unsigned char a = rgba[i * 4 + 3];

        /* Alpha-composite over black background */
        if (a < 255) {
            r = (unsigned char)((r * a + 0) / 255);
            g = (unsigned char)((g * a + 0) / 255);
            b = (unsigned char)((b * a + 0) / 255);
        }

        uint16_t pixel = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        if (swap_bytes) {
            pixel = (uint16_t)((pixel >> 8) | (pixel << 8));
        }
        rgb565[i] = pixel;
    }
}

static esp_err_t rasterize_image(NSVGimage *image,
                                  int w,
                                  int h,
                                  uint16_t **out_buf,
                                  bool swap_bytes)
{
    if (!image || !out_buf || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Compute scale to fit within (w x h) preserving aspect ratio */
    float sx = (float)w / image->width;
    float sy = (float)h / image->height;
    float scale = (sx < sy) ? sx : sy;

    /* Centre the image */
    float tx = ((float)w - image->width * scale) * 0.5f;
    float ty = ((float)h - image->height * scale) * 0.5f;

    /* Allocate RGBA buffer -- prefer PSRAM for large allocations */
    size_t rgba_size = (size_t)w * h * 4;
    unsigned char *rgba = (unsigned char *)heap_caps_calloc(1, rgba_size,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgba) {
        rgba = (unsigned char *)calloc(1, rgba_size);
    }
    if (!rgba) {
        ESP_LOGE(TAG, "failed to allocate RGBA buffer (%u bytes)", (unsigned)rgba_size);
        return ESP_ERR_NO_MEM;
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        ESP_LOGE(TAG, "failed to create rasterizer");
        free(rgba);
        return ESP_ERR_NO_MEM;
    }

    nsvgRasterize(rast, image, tx, ty, scale, rgba, w, h, w * 4);
    nsvgDeleteRasterizer(rast);

    /* Allocate RGB565 output buffer */
    size_t rgb565_count = (size_t)w * h;
    uint16_t *rgb565 = (uint16_t *)heap_caps_malloc(rgb565_count * sizeof(uint16_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb565) {
        rgb565 = (uint16_t *)malloc(rgb565_count * sizeof(uint16_t));
    }
    if (!rgb565) {
        ESP_LOGE(TAG, "failed to allocate RGB565 buffer (%u bytes)", (unsigned)(rgb565_count * 2));
        free(rgba);
        return ESP_ERR_NO_MEM;
    }

    rgba_to_rgb565(rgba, rgb565, (int)rgb565_count, swap_bytes);
    free(rgba);

    *out_buf = rgb565;
    return ESP_OK;
}

esp_err_t nanosvg_rasterize_file_rgb565(const char *path,
                                         int w,
                                         int h,
                                         uint16_t **out_buf,
                                         bool swap_bytes)
{
    if (!path || !out_buf) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t svg_len = 0;
    char *svg_data = read_file_to_string(path, &svg_len);
    if (!svg_data) {
        return ESP_ERR_NOT_FOUND;
    }

    /* nsvgParse modifies the input string in-place */
    NSVGimage *image = nsvgParse(svg_data, "px", 96.0f);
    free(svg_data);

    if (!image) {
        ESP_LOGE(TAG, "nsvgParse failed for %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SVG %s: %.0f x %.0f px, rasterizing to %d x %d",
             path, image->width, image->height, w, h);

    esp_err_t err = rasterize_image(image, w, h, out_buf, swap_bytes);
    nsvgDelete(image);
    return err;
}

esp_err_t nanosvg_rasterize_buf_rgb565(const char *svg_data,
                                        int w,
                                        int h,
                                        uint16_t **out_buf,
                                        bool swap_bytes)
{
    if (!svg_data || !out_buf) {
        return ESP_ERR_INVALID_ARG;
    }

    /* nsvgParse modifies the string, so we need a mutable copy */
    size_t len = strlen(svg_data);
    char *copy = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = (char *)malloc(len + 1);
    }
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, svg_data, len + 1);

    NSVGimage *image = nsvgParse(copy, "px", 96.0f);
    free(copy);

    if (!image) {
        ESP_LOGE(TAG, "nsvgParse failed for in-memory SVG");
        return ESP_FAIL;
    }

    esp_err_t err = rasterize_image(image, w, h, out_buf, swap_bytes);
    nsvgDelete(image);
    return err;
}
