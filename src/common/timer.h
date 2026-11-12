/* 계약 · 변경 금지. 근거: SPEC §6. 통계 구현은 timer.c (Phase 1). */
#ifndef TIMER_H
#define TIMER_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 측정·경과시간은 CLOCK_MONOTONIC만 쓴다 (R5). */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) { perror("clock_gettime"); exit(EXIT_FAILURE); }
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

typedef struct { uint64_t *v; size_t n, cap; } samples_t;

void stats_add   (samples_t *s, uint64_t ns);
void stats_report(samples_t *s, uint64_t bytes_total, double wall_sec);
/* 출력: p50, p95, p99, max, mean, MB/s, img/s. 종료 시 qsort 후 백분위. */
#endif
