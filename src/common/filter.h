/* 계약 · 변경 금지. 연산 정의는 SPEC §6. 구현은 filter.c (Phase 1). */
#ifndef FILTER_H
#define FILTER_H
#include <stdint.h>

/* DECISION: 문서가 시그니처를 정하지 않아 가장 단순한 형태로 둔다 —
 * GRAY는 src==dst 허용, BLUR/EDGE는 src/dst 분리. 3채널 고정.
 * 경계 픽셀은 clamp(가장자리 복제). dst는 w*h*3 바이트여야 한다. */
void filter_gray(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h);
void filter_blur(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h);
void filter_edge(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h);
#endif
