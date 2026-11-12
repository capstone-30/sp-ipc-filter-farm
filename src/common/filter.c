#include <stddef.h>
#include "filter.h"

/* 경계는 clamp(가장자리 복제). GRAY만 in-place 허용, 나머지는 별도 버퍼. 3채널 고정.
 * 관찰 대상은 IPC 비용이므로 SIMD·멀티코어 최적화는 하지 않는다 (SPEC §6). */

static inline uint32_t clampi(int32_t v, uint32_t hi)
{
    if (v < 0) return 0;
    return (uint32_t)v > hi ? hi : (uint32_t)v;
}

static inline const uint8_t *at(const uint8_t *src, uint32_t w, uint32_t h,
                                int32_t x, int32_t y)
{
    uint32_t cx = clampi(x, w - 1), cy = clampi(y, h - 1);
    return src + ((size_t)cy * w + cx) * 3;
}

static inline double luma(const uint8_t *p)
{
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
}

void filter_gray(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)
{
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        uint8_t y = (uint8_t)(luma(src + i * 3) + 0.5);   /* 반올림 */
        dst[i * 3] = dst[i * 3 + 1] = dst[i * 3 + 2] = y;
    }
}

void filter_blur(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sum[3] = { 0, 0, 0 };

            for (int32_t dy = -1; dy <= 1; dy++)
                for (int32_t dx = -1; dx <= 1; dx++) {
                    const uint8_t *p = at(src, w, h, (int32_t)x + dx, (int32_t)y + dy);
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                }

            uint8_t *q = dst + ((size_t)y * w + x) * 3;
            /* DECISION: 3×3 박스 평균은 절삭 나눗셈. 손계산이 쉽고 편향은 무시 가능. */
            q[0] = (uint8_t)(sum[0] / 9);
            q[1] = (uint8_t)(sum[1] / 9);
            q[2] = (uint8_t)(sum[2] / 9);
        }
}

void filter_edge(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)
{
    static const int32_t kx[9] = { -1, 0, 1, -2, 0, 2, -1, 0, 1 };
    static const int32_t ky[9] = { -1, -2, -1, 0, 0, 0, 1, 2, 1 };

    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            double gx = 0, gy = 0;

            for (int32_t dy = -1, k = 0; dy <= 1; dy++)
                for (int32_t dx = -1; dx <= 1; dx++, k++) {
                    double l = luma(at(src, w, h, (int32_t)x + dx, (int32_t)y + dy));
                    gx += kx[k] * l;
                    gy += ky[k] * l;
                }

            /* DECISION: libm 없이 round(sqrt(norm2))를 0..255에서 찾는다.
             * 반올림 경계 (v+0.5)^2 비교로 동일한 8bit 결과를 얻는다. */
            double norm2 = gx * gx + gy * gy;
            unsigned lo = 0, hi = 255;
            while (lo < hi) {
                unsigned mid = (lo + hi) / 2;
                double threshold = mid + 0.5;
                if (norm2 < threshold * threshold) hi = mid;
                else lo = mid + 1;
            }
            uint8_t v = (uint8_t)lo;

            uint8_t *q = dst + ((size_t)y * w + x) * 3;
            q[0] = q[1] = q[2] = v;
        }
}
