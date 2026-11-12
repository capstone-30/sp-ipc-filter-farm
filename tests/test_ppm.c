/* 저장 → 로드 왕복 시 바이트 단위 일치. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/common/ppm.h"

static void check_input(const char *path, const char *header, int valid)
{
    /* 공백·#·NUL은 래스터에서 모두 픽셀이다. */
    const uint8_t pixels[] = { '\n', '#', 0 };
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fputs(header, f) >= 0);
    assert(fwrite(pixels, 1, sizeof pixels, f) == sizeof pixels);
    assert(fclose(f) == 0);
    uint32_t w = 99, h = 99;
    errno = 0;
    uint8_t *px = ppm_load(path, &w, &h);
    if (valid) {
        assert(px != NULL && w == 1 && h == 1);
        assert(memcmp(px, pixels, sizeof pixels) == 0);
    } else {
        assert(px == NULL && errno == EINVAL);
        assert(w == 99 && h == 99);
    }
    free(px);
}

int main(void)
{
    const uint32_t W = 37, H = 23;        /* 정사각도 짝수도 아닌 크기 */
    char path[64];
    snprintf(path, sizeof path, "/tmp/ipcff.ppm.%d", (int)getpid());

    uint8_t *src = ppm_gen(W, H);
    assert(src != NULL);
    assert(ppm_save(path, src, W, H) == 0);

    uint32_t w = 0, h = 0;
    uint8_t *back = ppm_load(path, &w, &h);
    assert(back != NULL);
    assert(w == W && h == H);
    assert(memcmp(src, back, (size_t)W * H * 3) == 0);

    free(back);
    free(src);
    check_input(path, "P6\n1 1\n255\n", 1);
    check_input(path, "P6\n# comment\n1\t1\n255 ", 1);
    const char *bad[] = {
        "P61 1\n255\n", "P6\n1x1\n255\n", "P6\n1 1\n255x",
        "P6\n0 1\n255\n", "P6\n2049 1\n255\n", "P6\n1 2049\n255\n",
        "P6\n4294967296 1\n255\n", "P6\n99999999999999999999999 1\n255\n",
        "P6\n1 1\n256\n", "P6\n2 1\n255\n", "P3\n1 1\n255\n"
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) check_input(path, bad[i], 0);
    errno = 0;
    assert(ppm_gen(UINT32_MAX, UINT32_MAX) == NULL && errno == EINVAL);
    errno = 0;
    assert(ppm_save(path, NULL, 1, 1) == -1 && errno == EINVAL);
    assert(unlink(path) == 0);
    printf("test_ppm: OK (%ux%u 왕복·잘못된 입력 거절·픽셀 보존)\n", W, H);
    return 0;
}
