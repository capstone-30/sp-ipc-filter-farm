/* 계약 · 변경 금지. 근거: SPEC §6. 구현은 ppm.c (Phase 1). */
#ifndef PPM_H
#define PPM_H
#include <stdint.h>

/* P6 전용. 헤더 "P6\n<w> <h>\n255\n" + w*h*3 바이트. 반환 버퍼는 호출자 소유. */
uint8_t *ppm_load(const char *path, uint32_t *w, uint32_t *h);
int      ppm_save(const char *path, const uint8_t *px, uint32_t w, uint32_t h);
uint8_t *ppm_gen (uint32_t w, uint32_t h);   /* 벤치용 합성 이미지 */
#endif
