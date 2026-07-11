/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "image_utils.h"
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cstdlib>

static bool get_bmp_size_mem(const uint8_t *data, size_t len, int *width, int *height)
{
    if (len < 26) return false;
    if (data[0] != 'B' || data[1] != 'M') return false;

    *width  = (int32_t)(data[18] | (data[19] << 8) | (data[20] << 16) | (data[21] << 24));
    *height = (int32_t)(data[22] | (data[23] << 8) | (data[24] << 16) | (data[25] << 24));

    if (*height < 0) *height = -*height;
    return (*width > 0 && *height > 0);
}

static bool get_png_size_mem(const uint8_t *data, size_t len, int *width, int *height)
{
    if (len < 24) return false;
    const uint8_t png_sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(data, png_sig, 8) != 0) return false;

    *width  = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
    *height = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
    return (*width > 0 && *height > 0);
}

static bool get_jpg_size_mem(const uint8_t *data, size_t len, int *width, int *height)
{
    if (len < 2) return false;
    if (data[0] != 0xFF || data[1] != 0xD8) return false;

    size_t pos = 2;
    while (pos < len) {
        while (pos < len && data[pos] != 0xFF) pos++;
        if (pos >= len) break;
        pos++;

        while (pos < len && data[pos] == 0xFF) pos++;
        if (pos >= len) break;

        uint8_t marker = data[pos++];

        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (pos + 7 > len) break;
            *height = (data[pos + 3] << 8) | data[pos + 4];
            *width  = (data[pos + 5] << 8) | data[pos + 6];
            return (*width > 0 && *height > 0);
        }

        if (pos + 2 > len) break;
        uint16_t seg_len = (data[pos] << 8) | data[pos + 1];
        if (seg_len < 2) break;
        pos += seg_len;
    }
    return false;
}

bool get_image_size_from_memory(const uint8_t *data, size_t len, int *width, int *height, const char *ext)
{
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return get_jpg_size_mem(data, len, width, height);
    } else if (strcasecmp(ext, ".bmp") == 0) {
        return get_bmp_size_mem(data, len, width, height);
    } else if (strcasecmp(ext, ".png") == 0) {
        return get_png_size_mem(data, len, width, height);
    }
    return false;
}

bool get_image_size_from_file(const char *path, int *width, int *height)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    constexpr size_t BUF_SIZE = 65536;
    uint8_t *buf              = (uint8_t *)malloc(BUF_SIZE);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t n = fread(buf, 1, BUF_SIZE, f);
    fclose(f);

    if (n == 0) {
        free(buf);
        return false;
    }

    bool ok = false;
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        ok = get_jpg_size_mem(buf, n, width, height);
    } else if (strcasecmp(dot, ".bmp") == 0) {
        ok = get_bmp_size_mem(buf, n, width, height);
    } else if (strcasecmp(dot, ".png") == 0) {
        ok = get_png_size_mem(buf, n, width, height);
    }
    free(buf);
    return ok;
}
