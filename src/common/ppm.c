#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "ppm.h"
#include "proto.h"

/* P6만 지원. 헤더 "P6\n<w> <h>\n255\n" + w*h*3 바이트. */

static int read_uint(FILE *f, uint32_t *out)
{
    int c;
    do {                                        /* 공백과 # 주석 건너뛰기 */
        c = fgetc(f);
        if (c == '#') while (c != '\n' && c != EOF) c = fgetc(f);
    } while (isspace(c));

    if (c < '0' || c > '9') { if (!ferror(f)) errno = EINVAL; return -1; }

    uint32_t v = 0;
    do {
        uint32_t digit = (uint32_t)(c - '0');
        if (v > (UINT32_MAX - digit) / 10) { errno = EINVAL; return -1; }
        v = v * 10 + digit;
        c = fgetc(f);
    } while (c >= '0' && c <= '9');

    if (!isspace(c)) { if (!ferror(f)) errno = EINVAL; return -1; }
    *out = v;
    return 0;
}

static int check_dims(uint32_t w, uint32_t h, size_t *bytes)
{
    if (w == 0 || h == 0 || w > 2048 || h > 2048) { errno = EINVAL; return -1; }
    uint64_t n = (uint64_t)w * h * 3;           /* 64bit 곱으로 overflow 회피 */
    if (n > MAX_PAYLOAD) { errno = EINVAL; return -1; }
    *bytes = (size_t)n;
    return 0;
}

uint8_t *ppm_load(const char *path, uint32_t *w, uint32_t *h)
{
    if (path == NULL || w == NULL || h == NULL) { errno = EINVAL; return NULL; }
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;

    uint8_t *px = NULL;
    uint32_t maxval = 0, width = 0, height = 0;
    size_t bytes = 0;

    if (fgetc(f) != 'P' || fgetc(f) != '6' || !isspace(fgetc(f))) {
        if (!ferror(f)) errno = EINVAL;
        goto out;
    }
    if (read_uint(f, &width) != 0 || read_uint(f, &height) != 0) goto out;
    if (read_uint(f, &maxval) != 0) goto out;
    if (maxval != 255) { errno = EINVAL; goto out; }
    /* read_uint이 값 뒤 공백 1바이트를 이미 소비했다 */
    if (check_dims(width, height, &bytes) != 0) goto out;

    px = malloc(bytes);
    if (px != NULL && fread(px, 1, bytes, f) != bytes) {
        if (!ferror(f)) errno = EINVAL;
        free(px); px = NULL;
    }
out:
    {
        int saved = errno;
        if (fclose(f) != 0 && px != NULL) { free(px); return NULL; }
        if (px == NULL) errno = saved;
        else { *w = width; *h = height; }
    }
    return px;
}

int ppm_save(const char *path, const uint8_t *px, uint32_t w, uint32_t h)
{
    if (path == NULL || px == NULL) { errno = EINVAL; return -1; }
    size_t bytes;
    if (check_dims(w, h, &bytes) != 0) return -1;

    FILE *f = fopen(path, "wb");
    if (f == NULL) return -1;

    int ok = fprintf(f, "P6\n%u %u\n255\n", w, h) > 0
          && fwrite(px, 1, bytes, f) == bytes;

    int saved = errno;
    if (fclose(f) != 0) { if (ok) saved = errno; ok = 0; }
    if (!ok) errno = saved;
    return ok ? 0 : -1;
}

uint8_t *ppm_gen(uint32_t w, uint32_t h)
{
    size_t bytes;
    if (check_dims(w, h, &bytes) != 0) return NULL;

    uint8_t *px = malloc(bytes);
    if (px == NULL) return NULL;

    /* 그라데이션 + 격자. blur/edge가 실제로 할 일이 있어야 한다. */
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * w + x) * 3;
            uint8_t grid = (uint8_t)(((x & 31u) < 2u || (y & 31u) < 2u) ? 255 : 0);
            p[0] = (uint8_t)(x * 255u / (w > 1 ? w - 1 : 1));
            p[1] = (uint8_t)(y * 255u / (h > 1 ? h - 1 : 1));
            p[2] = grid;
        }
    return px;
}
