/* 3×3 고정 입력의 손계산 기대값과 비교. 골든 파일을 쓰지 않는다 —
 * 그 파일을 무엇으로 만드느냐가 다시 문제가 된다. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/common/filter.h"

static uint8_t src[27], dst[27];

static void fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 9; i++) { src[i*3] = r; src[i*3+1] = g; src[i*3+2] = b; }
}

int main(void)
{
    /* 1) gray: 0.299*100 + 0.587*150 + 0.114*200 = 140.75 → 반올림 141 */
    fill(100, 150, 200);
    filter_gray(src, dst, 3, 3);
    for (int i = 0; i < 9; i++)
        assert(dst[i*3] == 141 && dst[i*3+1] == 141 && dst[i*3+2] == 141);

    uint8_t original[256 * 3], separated[256 * 3], inplace[256 * 3];
    for (unsigned i = 0; i < 256; i++) {
        original[i * 3] = (uint8_t)i;
        original[i * 3 + 1] = (uint8_t)(255 - i);
        original[i * 3 + 2] = (uint8_t)(i * 73u);
    }
    memcpy(inplace, original, sizeof inplace);
    filter_gray(original, separated, 256, 1);
    filter_gray(inplace, inplace, 256, 1);
    assert(!memcmp(inplace, separated, sizeof inplace));

    /* 2) blur: 가운데만 90. clamp 복제 때문에 어느 출력 픽셀의 3×3 이웃도
     *    중앙을 정확히 한 번 포함한다 → 모든 출력이 90/9 = 10. */
    memset(src, 0, sizeof src);
    src[4*3] = src[4*3+1] = src[4*3+2] = 90;
    filter_blur(src, dst, 3, 3);
    for (int i = 0; i < 9; i++)
        assert(dst[i*3] == 10 && dst[i*3+1] == 10 && dst[i*3+2] == 10);

    /* 3) edge: 균일 입력 → 기울기 0 */
    fill(128, 128, 128);
    filter_edge(src, dst, 3, 3);
    for (int i = 0; i < 27; i++) assert(dst[i] == 0);

    /* 4) edge: 오른쪽 열만 255인 수직 계단.
     *    중앙에서 Gx = 255 + 2*255 + 255 = 1020, Gy = 0 → clamp 255 */
    memset(src, 0, sizeof src);
    for (int y = 0; y < 3; y++)
        for (int c = 0; c < 3; c++) src[(y*3 + 2)*3 + c] = 255;
    filter_edge(src, dst, 3, 3);
    assert(dst[4*3] == 255 && dst[4*3+1] == 255 && dst[4*3+2] == 255);

    /* 비포화 대각 기울기: Gx/Gy=40 또는 80, magnitude 반올림. */
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
            for (int c = 0; c < 3; c++) src[(y * 3 + x) * 3 + c] = (uint8_t)((x + y) * 10);
    const uint8_t expected[] = {57, 89, 57, 89, 113, 89, 57, 89, 57};
    filter_edge(src, dst, 3, 3);
    for (int i = 0; i < 9; i++)
        for (int c = 0; c < 3; c++) assert(dst[i * 3 + c] == expected[i]);
    printf("test_filter: OK (gray/blur/edge 손계산 일치)\n");
    return 0;
}
